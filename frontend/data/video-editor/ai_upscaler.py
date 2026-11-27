"""Local, streaming Real-ESRGAN worker for OBS Video Editor."""

import json
import sys
import time
from fractions import Fraction
from pathlib import Path


def report(**event):
    print(json.dumps(event), flush=True)


def load_model(options):
    import torch
    from spandrel import ImageModelDescriptor, ModelLoader

    torch.set_num_threads(min(4, torch.get_num_threads()))
    device = options["device"]
    if device == "auto":
        device = "cuda" if torch.cuda.is_available() else "cpu"
    if device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA is unavailable in this PyTorch installation.")
    weights = torch.load(options["model"], map_location="cpu", weights_only=True)
    descriptor = ModelLoader().load_from_state_dict(weights)
    if not isinstance(descriptor, ImageModelDescriptor):
        raise ValueError("Select a supported single-image super-resolution checkpoint.")
    if descriptor.scale not in (2, 3, 4) or descriptor.input_channels != 3 or descriptor.output_channels != 3:
        raise ValueError("The checkpoint must provide 2×, 3× or 4× RGB super-resolution.")
    descriptor.to(device=device).eval()
    label = torch.cuda.get_device_name(0) if device == "cuda" else "CPU"
    report(device=label, backend=device, scale=descriptor.scale)
    return torch, descriptor, device


def upscale(image, torch, model, device, tile, batch):
    import numpy as np
    from PIL import Image

    pixels = np.asarray(image)
    height, width = pixels.shape[:2]
    scale, overlap = model.scale, 24
    output = np.empty((height * scale, width * scale, 3), dtype=np.uint8)
    patches = [(x, y) for y in range(0, height, tile) for x in range(0, width, tile)]
    with torch.inference_mode():
        for index in range(0, len(patches), batch):
            tensors, regions = [], []
            for x, y in patches[index:index + batch]:
                w, h = min(tile, width - x), min(tile, height - y)
                padded = np.pad(
                    pixels[max(y - overlap, 0):min(y + h + overlap, height),
                           max(x - overlap, 0):min(x + w + overlap, width)],
                    ((max(overlap - y, 0), max(y + tile + overlap - height, 0)),
                     (max(overlap - x, 0), max(x + tile + overlap - width, 0)), (0, 0)),
                    mode="edge",
                )
                tensors.append(torch.from_numpy(padded).permute(2, 0, 1))
                regions.append((x, y, w, h))
            incoming = torch.stack(tensors).to(device, dtype=torch.float32).div_(255)
            prediction = model(incoming).mul(255).round().clamp_(0, 255).to(torch.uint8)
            border = overlap * scale
            prediction = prediction[:, :, border:border + tile * scale, border:border + tile * scale]
            patches_rgb = prediction.permute(0, 2, 3, 1).cpu().numpy()
            for patch, (x, y, w, h) in zip(patches_rgb, regions):
                output[y*scale:(y+h)*scale, x*scale:(x+w)*scale] = patch[:h*scale, :w*scale]
    return Image.fromarray(output)


def deblock(frame):
    import av

    graph = av.filter.Graph()
    source = graph.add_buffer(template=frame)
    filter_context = graph.add("deblock", "filter=weak:block=8")
    sink = graph.add("buffersink")
    source.link_to(filter_context)
    filter_context.link_to(sink)
    graph.configure()
    source.push(frame)
    return sink.pull()


def geometry(image, sar, target, framing):
    from PIL import Image, ImageOps

    width, height = target
    display_width = max(2, round(image.width * sar))
    if display_width != image.width:
        image = image.resize((display_width, image.height), Image.Resampling.LANCZOS)
    if framing == "crop":
        return ImageOps.fit(image, (width, height), Image.Resampling.LANCZOS)
    if framing == "pad":
        return ImageOps.pad(image, (width, height), Image.Resampling.LANCZOS, color="black")
    return image.resize((width, height), Image.Resampling.LANCZOS)


def output_size(image, sar, options):
    width, height = image.size
    aspect = float(width * sar / height)
    height_option = options["height"]
    if height_option == 0:
        return max(2, round(width * sar / 2) * 2), max(2, round(height / 2) * 2)
    box = (height_option * 16 // 9, height_option)
    if aspect < 1:
        box = box[::-1]
    box = tuple(value // 2 * 2 for value in box)
    if options["framing"] != "fit":
        return box
    width = min(box[0], box[1] * aspect)
    return max(2, round(width / 2) * 2), max(2, round(width / aspect / 2) * 2)


def prepare(frame, options, rotation):
    from PIL import ImageFilter

    if options["deblock"]:
        frame = deblock(frame)
    image = frame.to_image()
    if rotation:
        image = image.rotate(rotation, expand=True)
    if options["noise"]:
        image = image.filter(ImageFilter.MedianFilter(3))
    if options["antialias"]:
        image = image.filter(ImageFilter.GaussianBlur(0.45))
    return image


def finish(image, options):
    from PIL import ImageEnhance, ImageFilter

    if options["details"]:
        image = image.filter(ImageFilter.UnsharpMask(radius=1.6, percent=24, threshold=5))
    if options["sharpness"]:
        image = image.filter(ImageFilter.UnsharpMask(radius=0.7, percent=32, threshold=5))
    if options["contrast"]:
        image = ImageEnhance.Contrast(image).enhance(1.06)
    if options["color"]:
        image = ImageEnhance.Color(image).enhance(1.08)
    return image


def selected_frames(source, stream, start, end):
    """Yield the frame covering the in-point, then every frame until the out-point."""
    if start > 0:
        source.seek(int(start / stream.time_base), stream=stream, backward=True)
    previous = None
    emitted = False
    for frame in source.decode(stream):
        if frame.pts is None:
            continue
        stamp = frame.pts * frame.time_base
        if stamp < start:
            previous = frame
            continue
        if not emitted and previous is not None and stamp > start:
            yield previous, Fraction(0)
        if stamp >= end:
            return
        yield frame, stamp - start if emitted or previous is not None else Fraction(0)
        previous, emitted = None, True
    if not emitted and previous is not None:
        yield previous, Fraction(0)


def run(options):
    import av

    torch, model, device = load_model(options)
    with av.open(options["source"], options={"protocol_whitelist": "file"}) as source:
        stream = next((item for item in source.streams.video
                       if not item.disposition & item.disposition.attached_pic), None)
        if stream is None:
            raise ValueError("The source has no video stream.")
        origin = Fraction(source.start_time or 0, av.time_base)
        start = origin + Fraction(options["startMs"], 1000)
        end = origin + Fraction(options["endMs"], 1000)
        preview = options["preview"]
        if preview:
            start = origin + Fraction(options["previewMs"], 1000)
        frames = selected_frames(source, stream, start, end)
        first, _ = next(frames, (None, None))
        if first is None:
            raise ValueError("No video frame was found in the selected range.")
        rotation = getattr(first, "rotation", 0)
        sar = stream.sample_aspect_ratio or Fraction(1)
        if rotation % 180:
            sar = 1 / sar
        original = first.to_image()
        if rotation:
            original = original.rotate(rotation, expand=True)
        size = output_size(original, sar, options)
        if size[0] * size[1] > 4096 * 4096 or original.width * original.height * model.scale**2 > 150_000_000:
            raise ValueError("The selected source and model exceed the supported frame size.")

        def enhance(frame):
            image = prepare(frame, options, rotation)
            image = upscale(image, torch, model, device, options["tile"], options["batch"])
            return finish(geometry(image, sar, size, options["framing"]), options)

        if preview:
            destination = Path(options["output"]).parent
            geometry(original, sar, size, options["framing"]).save(destination / "original.png")
            enhance(first).save(destination / "enhanced.png")
            return
        encode(options, stream, first, frames, size, enhance, torch, device)


def encode(options, stream, first, frames, size, enhance, torch, device):
    import av
    from itertools import chain

    duration = Fraction(options["endMs"] - options["startMs"], 1000)
    requested_rate = options["fps"]
    rate = Fraction(requested_rate) if requested_rate else stream.average_rate or stream.guessed_rate or Fraction(30)
    time_base = Fraction(1, 90000)
    started = time.monotonic()
    total = stream.frames if options["startMs"] == 0 and options["whole"] and stream.frames else None
    codec = options["codec"]
    with av.open(options["output"], "w", format="matroska") as destination:
        output = destination.add_stream(codec, rate=rate)
        output.width, output.height = size
        output.pix_fmt = "yuv420p"
        output.codec_context.time_base = time_base
        output.codec_context.max_b_frames = 0
        output.codec_context.thread_count = 4
        if options["bitrate"]:
            output.bit_rate = options["bitrate"] * 1000
        elif codec in ("libx264", "libx265"):
            output.options = {"crf": str(options["quality"]), "preset": "fast"}
        pending = None
        last_pts = -1

        def write_packets(packets):
            nonlocal pending
            for packet in packets:
                if pending is not None:
                    pending.duration = max(1, packet.pts - pending.pts)
                    destination.mux(pending)
                pending = packet

        def write_frame(image, stamp):
            nonlocal last_pts
            candidate = round(stamp / time_base)
            if candidate <= last_pts:
                return
            last_pts = candidate
            result = av.VideoFrame.from_image(image)
            result.pts, result.time_base = candidate, time_base
            write_packets(output.encode(result))

        next_stamp, previous_image = Fraction(0), None
        for count, (frame, stamp) in enumerate(chain(((first, Fraction(0)),), frames), 1):
            if requested_rate:
                while previous_image is not None and next_stamp < stamp:
                    write_frame(previous_image, next_stamp)
                    next_stamp += 1 / rate
                previous_image = enhance(frame) if next_stamp < duration else None
            else:
                write_frame(enhance(frame), stamp)
            elapsed = time.monotonic() - started
            allocated = torch.cuda.memory_reserved() // (1024 * 1024) if device == "cuda" else None
            report(frames=count, total=total, percent=min(99, int(100 * stamp / duration)),
                   speed=round(count / max(elapsed, 0.001), 2), memoryMiB=allocated,
                   eta=round(elapsed * float(duration / stamp - 1)) if stamp > 0 and count >= 5 else None)
        while previous_image is not None and next_stamp < duration:
            write_frame(previous_image, next_stamp)
            next_stamp += 1 / rate
        write_packets(output.encode())
        if pending is not None:
            pending.duration = max(1, round(duration / pending.time_base) - pending.pts)
            destination.mux(pending)


if __name__ == "__main__":
    try:
        with open(sys.argv[1], encoding="utf-8") as settings:
            run(json.load(settings))
    except Exception as error:
        print(f"{type(error).__name__}: {error}", file=sys.stderr, flush=True)
        sys.exit(1)
