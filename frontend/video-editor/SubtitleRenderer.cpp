#include "SubtitleRenderer.hpp"

#include <QPainter>
#include <QRegularExpression>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QtMath>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace VideoEditor {

QImage RenderSubtitles(const QVector<Caption> &captions)
{
	QImage image;
	if (!captions.isEmpty()) {
		image = QImage(640, 360, QImage::Format_RGBA8888);
		image.fill(Qt::transparent);
		QPainter painter(&image);
		painter.setRenderHint(QPainter::TextAntialiasing);
		int stacked = 0;
		for (const auto &caption : captions) {
			const auto &style = caption.style;

			painter.setOpacity(caption.opacity / 100.0);
			QFont font = painter.font();
			if (const auto family = style["font"_L1].toString(); !family.isEmpty())
				font.setFamily(family);
			font.setPixelSize(std::clamp(style["size"_L1].toInt(23), 8, 100));
			font.setBold(style["bold"_L1].toBool(true));
			font.setItalic(style["italic"_L1].toBool());
			font.setLetterSpacing(QFont::AbsoluteSpacing, style["spacing"_L1].toDouble());
			QTextLayout layout(caption.text, font);
			QTextOption option;
			option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
			layout.setTextOption(option);
			QTextCharFormat format;
			format.setForeground(QColor(style["color"_L1].toString(u"#ffffff"_s)));
			if (const int outline = style["outline"_L1].toInt(); outline > 0)
				format.setTextOutline(QPen(Qt::black, outline, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			QList<QTextLayout::FormatRange> formats{{0, int(caption.text.size()), format}};
			static const QRegularExpression word(u"\\S+"_s);
			auto matches = word.globalMatch(caption.text);
			for (int i = 0; matches.hasNext() && caption.word >= 0; ++i) {
				const auto match = matches.next();
				if (i == caption.word) {
					format.setForeground(QColor(style["highlightColor"_L1].toString(u"#ffe34d"_s)));
					formats.append({int(match.capturedStart()), int(match.capturedLength()), format});
					break;
				}
			}
			layout.setFormats(formats);
			const int align = style["align"_L1].toInt(1);
			qreal height = 0;
			qreal width = 0;
			layout.beginLayout();
			for (auto line = layout.createLine(); line.isValid(); line = layout.createLine()) {
				line.setLineWidth(568);
				const auto extent = line.naturalTextWidth();
				line.setPosition({align == 0 ? 0 : (align == 2 ? -extent : -extent / 2), height});
				width = std::max(width, extent);
				height += line.height();
			}
			layout.endLayout();
			const int position = style["position"_L1].toInt(2);
			const auto x = position == 3 ? 6.4 * style["x"_L1].toInt(50) : align == 0 ? 36 : align == 2 ? 604 : 320;
			const auto y = position == 3 ? 3.6 * style["y"_L1].toInt(90) - height / 2
				       : position == 0 ? 26 + stacked
				       : position == 1 ? 180 - height / 2 - stacked : 332 - height - stacked;
			const QPointF origin(x, y + caption.offset);
			const QRectF bounds(origin.x() - (align == 0 ? 0 : align == 2 ? width : width / 2),
					    origin.y(), width, height);
			const int background = std::clamp(style["background"_L1].toInt(180), 0, 255);
			if (background)
				painter.fillRect(bounds.adjusted(-8, -4, 8, 4), QColor(0, 0, 0, background));
			if (const int shadow = style["shadow"_L1].toInt(); shadow > 0) {
				QTextCharFormat shade;
				shade.setForeground(QColor(0, 0, 0, 200));
				layout.draw(&painter, origin + QPointF(shadow, shadow),
					    {{0, int(caption.text.size()), shade}});
			}
			layout.draw(&painter, origin);
			stacked += qCeil(height) + 8;
		}
	}
	return image;
}

} // namespace VideoEditor
