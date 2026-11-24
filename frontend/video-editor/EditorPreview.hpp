#pragma once

#include <widgets/OBSQTDisplay.hpp>

class EditorPreview : public OBSQTDisplay {
public:
	explicit EditorPreview(QWidget *parent);
	~EditorPreview() override;

	bool Play(const QString &path, bool audio);
	void Clear();
	void Seek(int timeMs);
	int Time() const;
	bool Playing() const;

private:
	OBSSourceAutoRelease source;
	QString currentPath;

	static void Render(void *data, uint32_t width, uint32_t height);
	void Attach();
	void Detach();
};
