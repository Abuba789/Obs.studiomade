#pragma once

#include <widgets/OBSQTDisplay.hpp>

#include <QImage>
#include <QMutex>

class EditorPreview : public OBSQTDisplay {
public:
	explicit EditorPreview(QWidget *parent);
	~EditorPreview() override;

	bool Play(const QString &path, int timeMs);
	void Pause();
	void Clear();
	void Seek(int timeMs);
	int Time() const;
	bool Playing() const;
	bool Active() const { return source != nullptr; }
	void SetSubtitle(const QString &text);

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	OBSSourceAutoRelease source;
	QString currentPath;
	QString subtitleText;
	QMutex subtitleMutex;
	QImage subtitleImage;
	qint64 renderedSubtitle = 0;
	gs_texture_t *subtitleTexture = nullptr;

	static void Render(void *data, uint32_t width, uint32_t height);
	void RenderSubtitle(uint32_t width, uint32_t height);
	void Attach();
	void Detach();
};
