#pragma once

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QVector>

namespace VideoEditor {

enum class Track { Video, Audio };

struct SubtitleTiming {
	int inMs;
	int outMs;

	// Whisper can align only some words in a segment. A zero interval retains the word's index.
	bool Known() const { return outMs > inMs; }
};

struct Subtitle {
	int inMs;
	int outMs;
	QString text;
	QString language;
	QVector<SubtitleTiming> words;
	QJsonObject style{};
};

struct Asset {
	QString path;
	int durationMs;
	Track track;
	bool enhanced = false;
};

struct Clip {
	int asset;
	int startMs;
	int inMs;
	int outMs;
	QVector<Subtitle> subtitles;

	int Duration() const { return outMs - inMs; }
	int End() const { return startMs + Duration(); }
};

class Project {
public:
	const QVector<Asset> &Assets() const { return assets; }
	const QVector<Clip> &Clips() const { return clips; }
	int Duration() const;
	bool Import(const QString &path);
	bool AddEnhancedVersion(const QString &path, int clip, int sourceStart, bool replace);
	bool Append(int asset);
	bool Move(int clip, int startMs);
	bool Trim(int clip, int inMs, int outMs);
	bool Split(int clip, int sourceMs);
	bool SetSubtitles(int clip, QVector<Subtitle> subtitles);
	bool ExportSrt(const QString &path) const;
	bool ExportSubtitles(const QString &path) const;
	QStringList VideoExportArguments(const QString &directory, bool burnIn, QString &failedMedia) const;
	void Remove(int clip) { clips.removeAt(clip); }
	bool Open(const QString &path);
	bool Save(const QString &path) const;
	bool Export(const QString &directory) const;

private:
	QVector<Asset> assets;
	QVector<Clip> clips;

	bool CanPlace(const Clip &clip, int except) const;
	QJsonObject Serialize(const QString &directory) const;
	bool ExportSubtitleFile(const QString &path, const QString &format) const;
};

} // namespace VideoEditor
