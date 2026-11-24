#pragma once

#include <QString>
#include <QVector>

class QJsonObject;

namespace VideoEditor {

enum class Track { Video, Audio };

struct Asset {
	QString path;
	int durationMs;
	Track track;
};

struct Clip {
	int asset;
	int startMs;
};

class Project {
public:
	const QVector<Asset> &Assets() const { return assets; }
	const QVector<Clip> &Clips() const { return clips; }
	int Duration() const;
	bool Import(const QString &path);
	bool Append(int asset);
	bool Move(int clip, int startMs);
	void Remove(int clip) { clips.removeAt(clip); }
	bool Open(const QString &path);
	bool Save(const QString &path) const;
	bool Export(const QString &directory) const;

private:
	QVector<Asset> assets;
	QVector<Clip> clips;

	bool CanPlace(int asset, int startMs, int except) const;
	QJsonObject Serialize(const QString &directory) const;
};

} // namespace VideoEditor
