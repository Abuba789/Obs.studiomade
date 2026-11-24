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
		end = std::max(end, clip.startMs + assets[clip.asset].durationMs);
	return end;
}

bool Project::CanPlace(int asset, int startMs, int except) const
{
	const auto &media = assets[asset];
	if (startMs < 0 || startMs > std::numeric_limits<int>::max() - media.durationMs)
		return false;
	for (qsizetype i = 0; i < clips.size(); ++i) {
		const auto &other = clips[i];
		const auto &otherMedia = assets[other.asset];
		if (i != except && media.track == otherMedia.track && startMs < other.startMs + otherMedia.durationMs &&
		    other.startMs < startMs + media.durationMs)
			return false;
	}
	return true;
}

bool Project::Append(int asset)
{
	int start = 0;
	for (const auto &clip : clips) {
		const auto &media = assets[clip.asset];
		if (media.track == assets[asset].track)
			start = std::max(start, clip.startMs + media.durationMs);
	}
	if (!CanPlace(asset, start, -1))
		return false;
	clips.append({asset, start});
	return true;
}

bool Project::Move(int clip, int startMs)
{
	if (!CanPlace(clips[clip].asset, startMs, clip))
		return false;
	clips[clip].startMs = startMs;
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
	for (const auto &clip : clips)
		timeline.append(QJsonObject{{"asset"_L1, clip.asset}, {"startMs"_L1, clip.startMs}});
	return {{"version"_L1, 1}, {"media"_L1, media}, {"clips"_L1, timeline}};
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
	if (json["version"_L1].toInt() != 1 || !json["media"_L1].isArray() || !json["clips"_L1].isArray())
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
		if (asset < 0 || asset >= loaded.assets.size() || !loaded.CanPlace(asset, start, -1))
			return false;
		loaded.clips.append({asset, start});
	}
	*this = std::move(loaded);
	return true;
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
