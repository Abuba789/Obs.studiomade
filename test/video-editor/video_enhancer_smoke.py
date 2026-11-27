"""Integration tests using an official local Real-ESRGAN checkpoint.

Usage: python test/video-editor/video_enhancer_smoke.py FFMPEG FFPROBE MODEL [--cuda]
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def checked(*command):
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode:
        raise RuntimeError(f"{command[0]} failed:\n{result.stderr}")
    return result.stdout


def probe(probe_path, path):
    return json.loads(checked(probe_path, "-v", "error", "-show_streams", "-of", "json", str(path)))["streams"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ffmpeg")
    parser.add_argument("ffprobe")
    parser.add_argument("model", type=Path)
    parser.add_argument("--cuda", action="store_true")
    parser.add_argument("--quick", action="store_true", help="Run the low-resolution tests only.")
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    worker = Path(__file__).resolve().parents[2] / "frontend/data/video-editor/ai_upscaler.py"
    with tempfile.TemporaryDirectory(prefix="obs-enhancer-test-", dir=args.work_dir) as folder:
        folder = Path(folder).resolve()

        def fixture(name, video, duration):
            source = folder / (name + ".mp4")
            checked(args.ffmpeg, "-v", "error", "-f", "lavfi", "-i", video, "-f", "lavfi", "-i",
                    f"sine=frequency=440:duration={duration}", "-t", str(duration), "-c:v", "mpeg4", "-q:v", "8",
                    "-c:a", "aac", str(source))
            return source

        def options(name, source, duration, **changes):
            config = folder / (name + ".json")
            output = folder / (name + ".mkv")
            settings = dict(model=str(args.model.resolve()), device="cuda" if args.cuda else "cpu",
                            source=str(source), output=str(output), startMs=0, endMs=duration, preview=False,
                            previewMs=0, whole=True, height=0, framing="fit", tile=128, batch=1, codec="libx264",
                            fps=0, bitrate=0, quality=19, noise=False, sharpness=False, details=False,
                            deblock=False, contrast=False, color=False, antialias=False)
            settings.update(changes)
            config.write_text(json.dumps(settings), encoding="utf-8")
            return config, output

        def render(name, source, duration, size, **changes):
            config, video = options(name, source, duration, **changes)
            events = [json.loads(line) for line in checked(sys.executable, "-I", str(worker), str(config)).splitlines()]
            expected_device = "cuda" if args.cuda else "cpu"
            assert events[0]["backend"] == expected_device
            assert next(event for event in events if "frames" in event)["frames"] == 1
            output = folder / (name + ".mp4")
            checked(args.ffmpeg, "-v", "error", "-i", str(video), "-ss", str(changes.get("startMs", 0) / 1000),
                    "-i", str(source), "-map", "0:v:0", "-map", "1:a?", "-c:v", "copy", "-c:a", "aac",
                    "-t", str((duration - changes.get("startMs", 0)) / 1000), str(output))
            picture, audio = probe(args.ffprobe, output)
            assert (picture["width"], picture["height"]) == size
            assert picture["codec_type"] == "video" and audio["codec_type"] == "audio"
            assert abs(float(picture["duration"]) - float(audio["duration"])) < 0.12
            checked(args.ffmpeg, "-v", "error", "-i", str(output), "-f", "null", "-")
            print(f"{name}: {size[0]}×{size[1]}, audio and video decoded ({expected_device})", flush=True)

        if not args.quick:
            hd = fixture("720p", "testsrc2=size=1280x720:rate=4", 0.25)
            full_hd = fixture("1080p", "testsrc2=size=1920x1080:rate=4", 0.25)
            render("720-to-1080", hd, 250, (1920, 1080), height=1080)
            render("720-to-2160", hd, 250, (3840, 2160), height=2160)
            render("1080-to-2160", full_hd, 250, (3840, 2160), height=2160)
        moving = fixture("movement", "testsrc2=size=64x48:rate=60,noise=alls=20:allf=t", 0.3)
        render("noise-and-fast-motion", moving, 300, (64, 48), startMs=40, whole=False, noise=True,
               deblock=True, sharpness=True, details=True, contrast=True, color=True, antialias=True)
        for container in ("mkv", "mov"):
            output = folder / ("checked-container." + container)
            checked(args.ffmpeg, "-v", "error", "-i", str(folder / "noise-and-fast-motion.mkv"),
                    "-ss", "0.04", "-i", str(moving), "-map", "0:v:0", "-map", "1:a?", "-c:v", "copy",
                    "-c:a", "aac", "-t", "0.26", str(output))
            assert [stream["codec_type"] for stream in probe(args.ffprobe, output)] == ["video", "audio"]
            checked(args.ffmpeg, "-v", "error", "-i", str(output), "-f", "null", "-")
            print(f"{container.upper()} export: audio and video decoded", flush=True)
        config, _ = options("preview", moving, 300, preview=True, previewMs=100)
        checked(sys.executable, "-I", str(worker), str(config))
        assert (folder / "original.png").read_bytes() != (folder / "enhanced.png").read_bytes()
        print("Preview: original and enhanced images differ for the selected source frame", flush=True)

        long_video = fixture("long", "testsrc2=size=48x32:rate=1/8", 120)
        render("two-minute-source", long_video, 120000, (48, 32))
        original_hash = hashlib.sha256(long_video.read_bytes()).digest()
        config, _ = options("cancel", long_video, 120000)
        with subprocess.Popen([sys.executable, "-I", str(worker), str(config)], stdout=subprocess.PIPE, text=True) as child:
            try:
                first = json.loads(child.stdout.readline())
                assert first["backend"] in {"cpu", "cuda"}
            finally:
                child.kill()
                child.wait(timeout=10)
        assert child.returncode != 0 and hashlib.sha256(long_video.read_bytes()).digest() == original_hash
        print("Cancellation: worker stopped and source hash preserved", flush=True)


if __name__ == "__main__":
    main()
