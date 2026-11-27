#pragma once

#include <QImage>
#include <QJsonObject>
#include <QVector>

namespace VideoEditor {

struct Caption {
	QString text;
	QJsonObject style;
	int word;
	int opacity;
	int offset;

	bool operator==(const Caption &other) const
	{
		return text == other.text && style == other.style && word == other.word && opacity == other.opacity &&
		       offset == other.offset;
	}
};

QImage RenderSubtitles(const QVector<Caption> &captions);

} // namespace VideoEditor
