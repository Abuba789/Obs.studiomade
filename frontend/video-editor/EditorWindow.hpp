#pragma once

#include "EditorProject.hpp"
#include "SpeechRecognition.hpp"

#include <QMainWindow>

class EditorPreview;
class QDragEnterEvent;
class QDropEvent;
class QDoubleSpinBox;
class QGraphicsScene;
class QHideEvent;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QTimer;
class QUndoStack;
class QGraphicsLineItem;

class EditorWindow : public QMainWindow {
public:
	explicit EditorWindow(QWidget *parent);
	~EditorWindow() override;
	bool ConfirmProjectClose();

protected:
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dropEvent(QDropEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	VideoEditor::Project project;
	QString projectPath;
	EditorPreview *preview;
	QListWidget *library;
	QGraphicsScene *timeline;
	QLabel *details;
	QDoubleSpinBox *startTime;
	QDoubleSpinBox *inTime;
	QDoubleSpinBox *outTime;
	QSlider *seek;
	QTimer *previewTimer;
	QPushButton *append;
	QPushButton *move;
	QPushButton *remove;
	QPushButton *trim;
	QPushButton *split;
	QPushButton *play;
	int selectedClip = -1;
	bool modified = false;
	int selectedCue = -1;
	int jobClip = -1;
	int playheadSource = 0;
	QGraphicsLineItem *playhead = nullptr;
	QUndoStack *subtitleUndo;
	QPushButton *generate;
	QPushButton *enhance;
	QAction *addSubtitle;
	QAction *pasteSubtitle;
	QList<QAction *> cueActions;
	QAction *showSubtitles;
	SpeechRecognition *speechJob = nullptr;
	SpeechRecognition::Options speechOptions;
	VideoEditor::Subtitle copiedCue{};

	void Import();
	void ImportFiles(const QStringList &files);
	void Open();
	bool Save();
	void Export();
	void UpdateLibrary();
	void UpdateTimeline();
	void SelectClip(int clip);
	void UpdatePreviewPosition();
	void Play();
	void SetupSubtitles();
	void UpdateSubtitleActions();
	void AddSubtitleBlocks(double scale, int gutter);
	void UpdateSubtitlePreview(int sourceMs);
	void GenerateSubtitles();
	void AdjustSubtitleJob(int clip, int delta);
	void EditSubtitle(bool add);
	void EditSubtitleWord();
	void EditSubtitleStyle();
	void SplitSubtitle();
	void MergeSubtitle();
	void CopySubtitle();
	void PasteSubtitle();
	void ChangeSubtitles(int clip, QVector<VideoEditor::Subtitle> subtitles, int selection);
	void DragSubtitle(int clip, int cue, int mode, int delta);
	void ExportSubtitles();
	void ExportCaptionVideo();
	void EnhanceVideo();
	QString Text(const char *key) const;
};
