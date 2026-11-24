#pragma once

#include "EditorProject.hpp"

#include <QMainWindow>

class EditorPreview;
class QDoubleSpinBox;
class QGraphicsScene;
class QHideEvent;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;

class EditorWindow : public QMainWindow {
public:
	explicit EditorWindow(QWidget *parent);
	~EditorWindow() override;
	bool ConfirmProjectClose();

protected:
	void hideEvent(QHideEvent *event) override;

private:
	VideoEditor::Project project;
	QString projectPath;
	EditorPreview *preview;
	QListWidget *library;
	QGraphicsScene *timeline;
	QLabel *details;
	QDoubleSpinBox *startTime;
	QSlider *seek;
	QPushButton *append;
	QPushButton *move;
	QPushButton *remove;
	QPushButton *play;
	int selectedClip = -1;

	void Import();
	void Open();
	bool Save();
	void Export();
	void UpdateLibrary();
	void UpdateTimeline();
	void SelectClip(int clip);
	void Play();
	QString Text(const char *key) const;
};
