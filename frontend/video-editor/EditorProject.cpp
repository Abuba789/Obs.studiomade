#include "EditorProject.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

using namespace Qt::StringLiterals;

namespace VideoEditor {

static bool ValidSubtitles(const Clip &clip, const Asset &asset)
{
	for (const auto &cue : clip.subtitles) {
		if (cue.inMs < 0 || cue.outMs <= cue.inMs || cue.outMs > asset.durationMs || cue.text.trimmed().isEmpty())
			return false;
		for (const auto &word : cue.words) {
			if (word.inMs < 0 || word.outMs <= word.inMs || word.outMs > asset.durationMs)
				return false;
		}
	}
	return true;
}

bool Project::SetSubtitles(int clip, QVector<Subtitle> subtitles)
{
	auto updated = clips[clip];
	updated.subtitles = std::move(subtitles);
	if (!ValidSubtitles(updated, assets[updated.asset]))
		return false;
	clips[clip] = std::move(updated);
	return true;
}

bool Project::Import(const QString &path)
{
	const QString localPath = QFileInfo(path).canonicalFilePath();
	if (localPath.isEmpty() || !QFileInfo(localPath).isFile())
		return false;
	for (const auto &asset : assets) {
		if (asset.path == localPath)
			return true;
	}

	AVDictionary *options = nullptr;
	av_dict_set(&options, "protocol_whitelist", "file", 0);
	AVFormatContext *context = nullptr;
	const int opened = avformat_open_input(&context, localPath.toUtf8().constData(), nullptr, &options);
	av_dict_free(&options);
	if (opened < 0)
		return false;
	const auto closeInput = [](AVFormatContext *input) { avformat_close_input(&input); };
	const std::unique_ptr<AVFormatContext, decltype(closeInput)> input(context, closeInput);
	if (avformat_find_stream_info(input.get(), nullptr) < 0)
		return false;

	bool hasVideo = false;
	bool hasAudio = false;
	for (unsigned int i = 0; i < input->nb_streams; ++i) {
		const AVStream *stream = input->streams[i];
		const AVCodecParameters *codec = stream->codecpar;
		if (!avcodec_find_decoder(codec->codec_id))
			continue;
		hasVideo |= codec->codec_type == AVMEDIA_TYPE_VIDEO && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC);
		hasAudio |= codec->codec_type == AVMEDIA_TYPE_AUDIO;
	}
	const int64_t duration = input->duration / (AV_TIME_BASE / 1000);
	if ((!hasVideo && !hasAudio) || duration <= 0 || duration > std::numeric_limits<int>::max())
		return false;

	assets.append({localPath, static_cast<int>(duration), hasVideo ? Track::Video : Track::Audio});
	return true;
}

int Project::Duration() const
{
	int end = 0;
	for (const auto &clip : clips)
		end = std::max(end, clip.End());
	return end;
}

bool Project::CanPlace(const Clip &clip, int except) const
{
	if (clip.asset < 0 || clip.asset >= assets.size())
		return false;
	const auto &media = assets[clip.asset];
	if (clip.inMs < 0 || clip.outMs <= clip.inMs || clip.outMs > media.durationMs || clip.startMs < 0 ||
	    clip.startMs > std::numeric_limits<int>::max() - clip.Duration())
		return false;
	for (qsizetype i = 0; i < clips.size(); ++i) {
		const auto &other = clips[i];
		const auto &otherMedia = assets[other.asset];
		if (i != except && media.track == otherMedia.track && clip.startMs < other.End() &&
		    other.startMs < clip.End())
			return false;
	}
	return true;
}

bool Project::Append(int asset)
{
	if (asset < 0 || asset >= assets.size())
		return false;
	int start = 0;
	for (const auto &clip : clips) {
		const auto &media = assets[clip.asset];
		if (media.track == assets[asset].track)
			start = std::max(start, clip.End());
	}
	const Clip clip{asset, start, 0, assets[asset].durationMs, {}};
	if (!CanPlace(clip, -1))
		return false;
	clips.append(clip);
	return true;
}

bool Project::Move(int clip, int startMs)
{
	auto updated = clips[clip];
	updated.startMs = startMs;
	if (!CanPlace(updated, clip))
		return false;
	clips[clip] = updated;
	return true;
}

bool Project::Trim(int clip, int inMs, int outMs)
{
	auto updated = clips[clip];
	updated.inMs = inMs;
	updated.outMs = outMs;
	if (!CanPlace(updated, clip))
		return false;
	clips[clip] = updated;
	return true;
}

bool Project::Split(int clip, int sourceMs)
{
	auto left = clips[clip];
	if (sourceMs <= left.inMs || sourceMs >= left.outMs)
		return false;
	left.outMs = sourceMs;
	auto right = clips[clip];
	right.startMs = left.End();
	right.inMs = sourceMs;
	left.subtitles.removeIf([sourceMs](const Subtitle &cue) { return cue.inMs >= sourceMs; });
	right.subtitles.removeIf([sourceMs](const Subtitle &cue) { return cue.outMs <= sourceMs; });
	for (auto &cue : left.subtitles)
		cue.outMs = std::min(cue.outMs, sourceMs);
	for (auto &cue : right.subtitles)
		cue.inMs = std::max(cue.inMs, sourceMs);
	clips[clip] = left;
	clips.insert(clip + 1, right);
	return true;
}

QJsonObject Project::Serialize(const QString &directory) const
{
	const QDir base(directory);
	QJsonArray media;
	QJsonArray timeline;
	for (const auto &asset : assets) {
		media.append(QJsonObject{{"path"_L1, base.relativeFilePath(asset.path)},
					 {"durationMs"_L1, asset.durationMs},
					 {"track"_L1, asset.track == Track::Video ? "video"_L1 : "audio"_L1}});
	}
	for (const auto &clip : clips) {
		QJsonArray subtitles;
		for (const auto &cue : clip.subtitles) {
			QJsonArray words;
			for (const auto &word : cue.words)
				words.append(QJsonArray{word.inMs, word.outMs});
			subtitles.append(QJsonObject{{"inMs"_L1, cue.inMs}, {"outMs"_L1, cue.outMs},
						     {"text"_L1, cue.text}, {"language"_L1, cue.language}, {"words"_L1, words}});
		}
		timeline.append(QJsonObject{{"asset"_L1, clip.asset},
					    {"startMs"_L1, clip.startMs},
					    {"inMs"_L1, clip.inMs},
					    {"outMs"_L1, clip.outMs},
					    {"subtitles"_L1, subtitles}});
	}
	return {{"version"_L1, 3}, {"media"_L1, media}, {"clips"_L1, timeline}};
}

bool Project::Save(const QString &path) const
{
	const auto data = QJsonDocument(Serialize(QFileInfo(path).absolutePath())).toJson();
	QSaveFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}

bool Project::Open(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return false;
	const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
	const int version = json["version"_L1].toInt();
	if (version < 1 || version > 3 || !json["media"_L1].isArray() || !json["clips"_L1].isArray())
		return false;

	Project loaded;
	const QDir base(QFileInfo(path).absolutePath());
	for (const auto &entry : json["media"_L1].toArray()) {
		const auto item = entry.toObject();
		const QString mediaPath = item["path"_L1].toString();
		const int duration = item["durationMs"_L1].toInt();
		const QString track = item["track"_L1].toString();
		if (mediaPath.isEmpty() || duration <= 0 || (track != "video"_L1 && track != "audio"_L1))
			return false;
		loaded.assets.append({QDir::cleanPath(base.absoluteFilePath(mediaPath)), duration,
				      track == "video"_L1 ? Track::Video : Track::Audio});
	}
	for (const auto &entry : json["clips"_L1].toArray()) {
		const auto item = entry.toObject();
		const int asset = item["asset"_L1].toInt(-1);
		const int start = item["startMs"_L1].toInt(-1);
		if (asset < 0 || asset >= loaded.assets.size())
			return false;
		Clip clip{asset, start, version == 1 ? 0 : item["inMs"_L1].toInt(-1),
			  version == 1 ? loaded.assets[asset].durationMs : item["outMs"_L1].toInt(-1), {}};
		for (const auto &subtitle : item["subtitles"_L1].toArray()) {
			const auto cue = subtitle.toObject();
			Subtitle value{cue["inMs"_L1].toInt(-1), cue["outMs"_L1].toInt(-1), cue["text"_L1].toString(),
				       cue["language"_L1].toString(), {}};
			for (const auto &timing : cue["words"_L1].toArray()) {
				const auto word = timing.toArray();
				if (word.size() != 2)
					return false;
				value.words.append({word[0].toInt(-1), word[1].toInt(-1)});
			}
			clip.subtitles.append(std::move(value));
		}
		if (!loaded.CanPlace(clip, -1) || !ValidSubtitles(clip, loaded.assets[asset]))
			return false;
		loaded.clips.append(clip);
	}
	*this = std::move(loaded);
	return true;
}

bool Project::ExportSrt(const QString &path) const
{
	QVector<Subtitle> visible;
	for (const auto &clip : clips) {
		for (auto cue : clip.subtitles) {
			const int start = std::max(cue.inMs, clip.inMs);
			const int end = std::min(cue.outMs, clip.outMs);
			if (start >= end)
				continue;
			cue.inMs = clip.startMs + (start - clip.inMs);
			cue.outMs = clip.startMs + (end - clip.inMs);
			visible.append(std::move(cue));
		}
	}
	std::stable_sort(visible.begin(), visible.end(), [](const Subtitle &a, const Subtitle &b) {
		return a.inMs < b.inMs;
	});
	const auto timestamp = [](int ms) {
		return QStringLiteral("%1:%2:%3,%4")
			.arg(ms / 3600000, 2, 10, QLatin1Char('0')).arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
			.arg(ms / 1000 % 60, 2, 10, QLatin1Char('0')).arg(ms % 1000, 3, 10, QLatin1Char('0'));
	};
	QSaveFile file(path);
	if (visible.isEmpty() || !file.open(QIODevice::WriteOnly))
		return false;
	for (qsizetype i = 0; i < visible.size(); ++i) {
		const auto &cue = visible[i];
		const auto data = QStringLiteral("%1\n%2 --> %3\n%4\n\n").arg(i + 1)
					  .arg(timestamp(cue.inMs), timestamp(cue.outMs), cue.text.simplified()).toUtf8();
		if (file.write(data) != data.size())
			return false;
	}
	return file.commit();
}

bool Project::Export(const QString &directory) const
{
	const QFileInfo destination(directory);
	if (destination.exists() || destination.fileName().isEmpty())
		return false;
	QTemporaryDir staging(destination.absolutePath() + "/.obs-editor-XXXXXX"_L1);
	if (!staging.isValid())
		return false;
	QDir folder(staging.path());
	if (!folder.mkdir("media"_L1))
		return false;

	Project portable = *this;
	for (qsizetype i = 0; i < portable.assets.size(); ++i) {
		auto &asset = portable.assets[i];
		const QString copy = folder.filePath(QStringLiteral("media/%1-%2").arg(i + 1).arg(QFileInfo(asset.path).fileName()));
		if (!QFile::copy(asset.path, copy))
			return false;
		asset.path = copy;
	}
	return portable.Save(folder.filePath("project.obsedit"_L1)) && QDir().rename(staging.path(), directory);
}

} // namespace VideoEditor
