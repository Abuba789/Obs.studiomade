#include "EditorProject.hpp"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>

#include <algorithm>
#include <limits>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

using namespace Qt::StringLiterals;

namespace VideoEditor {

bool Project::ExportSrt(const QString &path) const
{
	return ExportSubtitleFile(path, u"srt"_s);
}

bool Project::ExportSubtitles(const QString &path) const
{
	return ExportSubtitleFile(path, QFileInfo(path).suffix().toLower());
}

bool Project::ExportSubtitleFile(const QString &path, const QString &format) const
{
	const bool ass = format == "ass"_L1;
	const bool vtt = format == "vtt"_L1;
	QVector<Subtitle> visible;
	for (const auto &clip : clips) {
		for (auto cue : clip.subtitles) {
			const int start = std::max(cue.inMs, clip.inMs);
			const int end = std::min(cue.outMs, clip.outMs);
			if (start >= end)
				continue;
			for (auto &word : cue.words) {
				if (!word.Known())
					continue;
				word.inMs = std::clamp(word.inMs, start, end) - start;
				word.outMs = std::clamp(word.outMs, start, end) - start;
			}
			cue.inMs = clip.startMs + (start - clip.inMs);
			cue.outMs = clip.startMs + (end - clip.inMs);
			visible.append(std::move(cue));
		}
	}
	std::stable_sort(visible.begin(), visible.end(), [](const Subtitle &a, const Subtitle &b) {
		return a.inMs < b.inMs;
	});
	const auto timestamp = [ass, vtt](int ms) {
		return QStringLiteral("%1:%2:%3%4%5")
			.arg(ms / 3600000, ass ? 1 : 2, 10, u'0').arg(ms / 60000 % 60, 2, 10, u'0')
			.arg(ms / 1000 % 60, 2, 10, u'0').arg(ass || vtt ? u'.' : u',')
			.arg(ass ? ms % 1000 / 10 : ms % 1000, ass ? 2 : 3, 10, u'0');
	};
	if ((!ass && !vtt && format != "srt"_L1) || visible.isEmpty())
		return false;
	QString output;
	if (vtt)
		output = u"WEBVTT\n\n"_s;
	if (ass) {
		output = u"[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
			 u"WrapStyle: 0\nScaledBorderAndShadow: yes\n\n[V4+ Styles]\n"
			 u"Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
			 u"Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
			 u"Alignment, MarginL, MarginR, MarginV, Encoding\n"_s;
	}
	const auto color = [](QString value, const QString &fallback) {
		value.remove(u'#');
		bool valid;
		const auto rgb = value.toUInt(&valid, 16);
		if (!valid || value.size() != 6)
			return fallback;
		return QStringLiteral("&H%1").arg(((rgb & 255) << 16) | (rgb & 0xff00) | (rgb >> 16), 6, 16, u'0');
	};
	QString events;
	if (ass)
		events = u"\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"_s;
	for (qsizetype i = 0; i < visible.size(); ++i) {
		const auto &cue = visible[i];
		if (!ass) {
			auto text = cue.text.simplified();
			if (vtt)
				text = text.toHtmlEscaped();
			else
				output += QString::number(i + 1) + u'\n';
			output += QStringLiteral("%1 --> %2\n%3\n\n").arg(timestamp(cue.inMs), timestamp(cue.outMs), text);
			continue;
		}
		const auto &style = cue.style;
		QString font = style["font"_L1].toString(u"Arial"_s).simplified();
		font.replace(u',', u' ');
		const auto name = QStringLiteral("Cue%1").arg(i);
		const auto primary = color(style["color"_L1].toString(), u"&Hffffff"_s);
		const auto highlight = color(style["highlightColor"_L1].toString(), u"&H4de3ff"_s);
		const int align = std::clamp(style["align"_L1].toInt(1), 0, 2);
		const int position = style["position"_L1].toInt(2);
		const int alignment = 1 + align + (position == 0 ? 6 : position == 2 ? 0 : 3);
		const int background = std::clamp(style["background"_L1].toInt(180), 0, 255);
		const auto box = QStringLiteral("&H%1").arg(255 - background, 2, 16, u'0') + u"000000"_s;
		output += QStringLiteral("Style: %1,%2,%3,%4,%5,%6,&H37000000,%7,%8,0,0,100,100,%9,0,%10,%11,%12,%13,36,36,26,1\n")
				  .arg(name, font).arg(std::clamp(style["size"_L1].toInt(23), 8, 100))
				  .arg(primary, highlight, background ? box : u"&H00000000"_s)
				  .arg(style["bold"_L1].toBool(true) ? -1 : 0).arg(style["italic"_L1].toBool() ? -1 : 0)
				  .arg(style["spacing"_L1].toInt()).arg(background ? 3 : 1)
				  .arg(background ? 4 : style["outline"_L1].toInt()).arg(style["shadow"_L1].toInt())
				  .arg(alignment);
		const int duration = cue.outMs - cue.inMs;
		const int fadeIn = std::clamp(style["fadeIn"_L1].toInt(), 0, duration / 2);
		const int fadeOut = std::clamp(style["fadeOut"_L1].toInt(), 0, duration / 2);
		QString tags = QStringLiteral("\\fad(%1,%2)").arg(fadeIn).arg(fadeOut);
		const int x = position == 3 ? qRound(6.4 * style["x"_L1].toInt(50)) : align == 0 ? 36 : align == 2 ? 604 : 320;
		const int y = position == 3 ? qRound(3.6 * style["y"_L1].toInt(90)) : position == 0 ? 26 : position == 1 ? 180 : 334;
		if (style["slide"_L1].toBool())
			tags += QStringLiteral("\\move(%1,%2,%1,%3,0,%4)").arg(x).arg(y + 20).arg(y).arg(std::min(200, duration));
		else if (position == 3)
			tags += QStringLiteral("\\pos(%1,%2)").arg(x).arg(y);
		auto safe = cue.text.simplified();
		safe.replace(u'\\', u'＼').replace(u'{', u'｛').replace(u'}', u'｝');
		auto words = safe.split(u' ');
		if (style["highlight"_L1].toBool() && cue.words.size() == words.size()) {
			for (qsizetype j = 0; j < words.size(); ++j) {
				const auto &word = cue.words[j];
				if (word.outMs <= word.inMs)
					continue;
				words[j].prepend(QStringLiteral("{\\1c%1&\\t(%2,%3,\\1c%4&)\\t(%5,%6,\\1c%1&)}")
							 .arg(primary).arg(word.inMs).arg(word.inMs + 1).arg(highlight)
							 .arg(word.outMs - 1).arg(word.outMs));
			}
		}
		const int end = cue.outMs / 10 == cue.inMs / 10 ? (cue.outMs > std::numeric_limits<int>::max() - 10
									   ? cue.outMs : cue.outMs + 10) : cue.outMs;
		events += QStringLiteral("Dialogue: 0,%1,%2,%3,,0,0,0,,{%4}%5\n")
				  .arg(timestamp(cue.inMs), timestamp(end), name, tags, words.join(u' '));
	}
	output += events;
	QSaveFile file(path);
	const auto data = output.toUtf8();
	return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}


QStringList Project::VideoExportArguments(const QString &directory, bool burnIn, QString &failedMedia) const
{
	const QDir folder(directory);
	if (!ExportSubtitleFile(folder.filePath(u"captions.ass"_s), u"ass"_s))
		return {};
	const auto seconds = [](int ms) { return QString::number(ms / 1000.0, 'f', 3); };
	const auto duration = seconds(Duration());
	QStringList arguments{u"-nostdin"_s, u"-hide_banner"_s, u"-loglevel"_s, u"error"_s, u"-progress"_s,
			      u"pipe:1"_s, u"-filter_complex_threads"_s, u"2"_s};
	QStringList graph{QStringLiteral("color=c=black:s=1280x720:r=30:d=%1[base]").arg(duration),
			  QStringLiteral("anullsrc=r=48000:cl=stereo:d=%1[silence]").arg(duration)};
	QString video = u"[base]"_s;
	QString audio = u"[silence]"_s;
	int audioCount = 1;
	QHash<int, QPair<int, int>> streams;
	for (qsizetype i = 0; i < clips.size(); ++i) {
		const auto &clip = clips[i];
		const auto &asset = assets[clip.asset];
		if (!streams.contains(clip.asset)) {
			AVDictionary *options = nullptr;
			av_dict_set(&options, "protocol_whitelist", "file", 0);
			AVFormatContext *input = nullptr;
			const int opened = avformat_open_input(&input, asset.path.toUtf8().constData(), nullptr, &options);
			av_dict_free(&options);
			if (opened < 0) {
				failedMedia = asset.path;
				return {};
			}
			int picture = -1;
			int sound = -1;
			for (unsigned int j = 0; j < input->nb_streams; ++j) {
				const auto *stream = input->streams[j];
				const auto *codec = stream->codecpar;
				if (!avcodec_find_decoder(codec->codec_id))
					continue;
				if (picture < 0 && codec->codec_type == AVMEDIA_TYPE_VIDEO &&
				    !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
					picture = static_cast<int>(j);
				if (sound < 0 && codec->codec_type == AVMEDIA_TYPE_AUDIO)
					sound = static_cast<int>(j);
			}
			avformat_close_input(&input);
			if ((asset.track == Track::Video && picture < 0) || (asset.track == Track::Audio && sound < 0)) {
				failedMedia = asset.path;
				return {};
			}
			streams.insert(clip.asset, {picture, sound});
		}
		const auto [picture, sound] = streams.value(clip.asset);
		arguments += {u"-protocol_whitelist"_s, u"file"_s, u"-threads"_s, u"2"_s, u"-ss"_s, seconds(clip.inMs),
			      u"-t"_s, seconds(clip.Duration()), u"-i"_s, asset.path};
		if (asset.track == Track::Video) {
			graph.append(QStringLiteral("[%1:%2]fps=30,scale=1280:720:force_original_aspect_ratio=decrease:"
						    "force_divisible_by=2,pad=1280:720:(ow-iw)/2:(oh-ih)/2,setsar=1,"
						    "setpts=PTS+%3/TB[v%1]")
					     .arg(i).arg(picture).arg(seconds(clip.startMs)));
			const auto label = QStringLiteral("[layer%1]").arg(i);
			graph.append(QStringLiteral("%1[v%2]overlay=eof_action=pass:repeatlast=0:"
						    "enable='gte(t,%3)*lt(t,%4)'%5")
					     .arg(video).arg(i).arg(seconds(clip.startMs), seconds(clip.End()), label));
			video = label;
		}
		if (sound >= 0) {
			const auto label = QStringLiteral("[audio%1]").arg(i);
			graph.append(QStringLiteral("[%1:%2]aresample=48000:async=1:first_pts=0,"
						    "aformat=channel_layouts=stereo,adelay=%3:all=1%4")
					     .arg(i).arg(sound).arg(clip.startMs).arg(label));
			audio += label;
			++audioCount;
		}
	}
	graph.append(QStringLiteral("%1amix=inputs=%2:duration=first:normalize=0,alimiter=limit=0.95:latency=1[outa]")
			     .arg(audio).arg(audioCount));
	graph.append(video + (burnIn ? u"ass=captions.ass,format=yuv420p[outv]"_s : u"format=yuv420p[outv]"_s));
	QSaveFile script(folder.filePath(u"filters.txt"_s));
	const auto data = graph.join(u';').toUtf8();
	if (!script.open(QIODevice::WriteOnly) || script.write(data) != data.size() || !script.commit())
		return {};
	if (!burnIn)
		arguments += {u"-i"_s, u"captions.ass"_s};
	arguments += {u"-filter_complex_script"_s, u"filters.txt"_s, u"-map"_s, u"[outv]"_s, u"-map"_s, u"[outa]"_s,
		      u"-t"_s, duration, u"-c:v"_s, u"mpeg4"_s, u"-q:v"_s, u"3"_s, u"-c:a"_s, u"aac"_s};
	if (!burnIn)
		arguments += {u"-map"_s, QString::number(clips.size()) + u":s:0"_s, u"-c:s"_s, u"copy"_s};
	arguments.append(u"output.mkv"_s);
	return arguments;
}

} // namespace VideoEditor
