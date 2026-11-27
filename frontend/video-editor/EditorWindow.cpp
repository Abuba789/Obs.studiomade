#include "EditorWindow.hpp"
#include "EditorPreview.hpp"

#include <OBSApp.hpp>

#include <obs-frontend-api.h>
#include <util/bmem.h>

#include <QAction>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGroupBox>
#include <QHideEvent>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;
using VideoEditor::Track;

EditorWindow::EditorWindow(QWidget *parent) : QMainWindow(parent)
{
	setWindowFlags(Qt::Widget);
	setAcceptDrops(true);

	auto *toolbar = addToolBar(Text("Project"));
	toolbar->setMovable(false);
	connect(toolbar->addAction(Text("Open")), &QAction::triggered, this, &EditorWindow::Open);
	connect(toolbar->addAction(Text("Save")), &QAction::triggered, this, &EditorWindow::Save);
	connect(toolbar->addAction(Text("Export")), &QAction::triggered, this, &EditorWindow::Export);
	SetupSubtitles();

	auto *workspace = new QSplitter(Qt::Vertical, this);
	setCentralWidget(workspace);
	auto *panes = new QSplitter(Qt::Horizontal, workspace);

	auto *mediaPanel = new QGroupBox(Text("Library"), panes);
	auto *mediaLayout = new QVBoxLayout(mediaPanel);
	auto *import = new QPushButton(Text("Import"), mediaPanel);
	library = new QListWidget(mediaPanel);
	append = new QPushButton(Text("Add"), mediaPanel);
	mediaLayout->addWidget(import);
	mediaLayout->addWidget(library);
	mediaLayout->addWidget(append);
	connect(import, &QPushButton::clicked, this, &EditorWindow::Import);
	append->setEnabled(false);
	connect(library, &QListWidget::currentRowChanged, this, [this](int row) { append->setEnabled(row >= 0); });
	connect(append, &QPushButton::clicked, this, [this] {
		if (!project.Append(library->currentRow())) {
			statusBar()->showMessage(Text("InvalidPlacement"));
			return;
		}
		modified = true;
		SelectClip(static_cast<int>(project.Clips().size()) - 1);
		UpdateTimeline();
	});

	auto *previewPanel = new QGroupBox(Text("Preview"), panes);
	auto *previewLayout = new QVBoxLayout(previewPanel);
	preview = new EditorPreview(previewPanel);
	preview->setAccessibleName(Text("SubtitlePreview"));
	play = new QPushButton(Text("Play"), previewPanel);
	seek = new QSlider(Qt::Horizontal, previewPanel);
	seek->setAccessibleName(Text("Seek"));
	seek->setTracking(false);
	previewLayout->addWidget(preview, 1);
	previewLayout->addWidget(seek);
	previewLayout->addWidget(play);
	connect(play, &QPushButton::clicked, this, &EditorWindow::Play);
	connect(seek, &QSlider::valueChanged, this, [this](int offset) {
		if (selectedClip >= 0) {
			const int source = project.Clips()[selectedClip].inMs + offset;
			preview->Seek(source);
			UpdateSubtitlePreview(source);
		}
	});

	auto *properties = new QGroupBox(Text("Properties"), panes);
	auto *form = new QFormLayout(properties);
	details = new QLabel(properties);
	details->setWordWrap(true);
	details->setTextFormat(Qt::PlainText);
	details->setTextInteractionFlags(Qt::TextSelectableByMouse);
	startTime = new QDoubleSpinBox(properties);
	startTime->setDecimals(3);
	startTime->setMaximum(2147483.647);
	startTime->setSingleStep(1);
	move = new QPushButton(Text("Move"), properties);
	remove = new QPushButton(Text("Remove"), properties);
	inTime = new QDoubleSpinBox(properties);
	outTime = new QDoubleSpinBox(properties);
	for (auto *time : {inTime, outTime}) {
		time->setDecimals(3);
		time->setSingleStep(0.1);
	}
	trim = new QPushButton(Text("Trim"), properties);
	split = new QPushButton(Text("Split"), properties);
	generate = new QPushButton(Text("GenerateSubtitles"), properties);
	enhance = new QPushButton(Text("Enhancer"), properties);
	form->addRow(details);
	form->addRow(generate);
	form->addRow(enhance);
	connect(generate, &QPushButton::clicked, this, &EditorWindow::GenerateSubtitles);
	connect(enhance, &QPushButton::clicked, this, &EditorWindow::EnhanceVideo);
	form->addRow(Text("StartSeconds"), startTime);
	form->addRow(move);
	form->addRow(Text("InSeconds"), inTime);
	form->addRow(Text("OutSeconds"), outTime);
	form->addRow(trim);
	form->addRow(split);
	form->addRow(remove);
	connect(move, &QPushButton::clicked, this, [this] {
		const int start = static_cast<int>(qRound64(startTime->value() * 1000));
		if (!project.Move(selectedClip, start)) {
			statusBar()->showMessage(Text("InvalidPlacement"));
			return;
		}
		statusBar()->clearMessage();
		modified = true;
		UpdateTimeline();
	});
	connect(trim, &QPushButton::clicked, this, [this] {
		const int in = static_cast<int>(qRound64(inTime->value() * 1000));
		const int out = static_cast<int>(qRound64(outTime->value() * 1000));
		if (!project.Trim(selectedClip, in, out)) {
			statusBar()->showMessage(Text("InvalidTrim"), 5000);
			return;
		}
		modified = true;
		SelectClip(selectedClip);
		UpdateTimeline();
	});
	connect(split, &QPushButton::clicked, this, [this] {
		const int position = project.Clips()[selectedClip].inMs + seek->value();
		if (!project.Split(selectedClip, position)) {
			statusBar()->showMessage(Text("InvalidSplit"), 5000);
			return;
		}
		modified = true;
		AdjustSubtitleJob(selectedClip, 1);
		SelectClip(selectedClip + 1);
		subtitleUndo->clear();
		UpdateTimeline();
	});
	connect(remove, &QPushButton::clicked, this, [this] {
		AdjustSubtitleJob(selectedClip, -1);
		project.Remove(selectedClip);
		subtitleUndo->clear();
		modified = true;
		SelectClip(-1);
		UpdateTimeline();
	});

	auto *timelinePanel = new QGroupBox(Text("Timeline"), workspace);
	auto *timelineLayout = new QVBoxLayout(timelinePanel);
	timeline = new QGraphicsScene(this);
	auto *view = new QGraphicsView(timeline, timelinePanel);
	view->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	timelineLayout->addWidget(view);
	connect(timeline, &QGraphicsScene::selectionChanged, this, [this] {
		const auto items = timeline->selectedItems();
		const int clip = items.isEmpty() ? -1 : items.first()->data(0).toInt();
		if (clip != selectedClip)
			SelectClip(clip);
		const auto item = items.isEmpty() ? nullptr : items.first();
		selectedCue = item && item->data(1).isValid() ? item->data(1).toInt() : -1;
		if (selectedCue >= 0) {
			const auto &segment = project.Clips()[clip];
			const int offset = std::max(0, segment.subtitles[selectedCue].inMs - segment.inMs);
			const auto &asset = project.Assets()[segment.asset];
			if (QFileInfo(asset.path).isFile())
				preview->Play(asset.path, segment.inMs + offset, true);
			previewTimer->stop();
			play->setText(Text("Play"));
			seek->setValue(offset);
			UpdateSubtitlePreview(segment.inMs + offset);
		}
		UpdateSubtitleActions();
	});
	panes->setSizes({220, 600, 245});
	panes->setStretchFactor(1, 1);
	workspace->setSizes({470, 180});
	workspace->setStretchFactor(0, 1);

	previewTimer = new QTimer(this);
	previewTimer->setInterval(20);
	connect(previewTimer, &QTimer::timeout, this, &EditorWindow::UpdatePreviewPosition);
	SelectClip(-1);
	UpdateTimeline();
}

EditorWindow::~EditorWindow()
{
	delete speechJob;
	timeline->disconnect(this);
}

QString EditorWindow::Text(const char *key) const
{
	const QByteArray lookup = QByteArray("VideoEditor.") + key;
	return QTStr(lookup.constData());
}

void EditorWindow::Import()
{
	char *recordingPath = obs_frontend_get_current_record_output_path();
	const QString initialDirectory = QString::fromUtf8(recordingPath);
	bfree(recordingPath);
	ImportFiles(QFileDialog::getOpenFileNames(this, Text("Import"), initialDirectory, {}, nullptr,
						 QFileDialog::DontUseNativeDialog));
}

void EditorWindow::ImportFiles(const QStringList &files)
{
	const auto previous = project.Assets().size();
	QStringList failed;
	for (const auto &file : files) {
		if (!project.Import(file))
			failed.append(QFileInfo(file).fileName());
	}
	if (previous != project.Assets().size()) {
		modified = true;
		UpdateLibrary();
		library->setCurrentRow(static_cast<int>(previous));
	}
	if (!failed.isEmpty())
		QMessageBox::warning(this, Text("Project"), Text("ImportFailed").arg(failed.join(u'\n')));
}

bool EditorWindow::ConfirmProjectClose()
{
	if (!modified)
		return true;
	const auto answer = QMessageBox::question(this, Text("Project"), Text("SaveChanges"),
						  QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
	return answer == QMessageBox::Discard || (answer == QMessageBox::Save && Save());
}

void EditorWindow::Open()
{
	const QString path = QFileDialog::getOpenFileName(this, Text("Open"), {}, Text("ProjectFilter"));
	if (path.isEmpty() || !ConfirmProjectClose())
		return;
	if (!project.Open(path)) {
		QMessageBox::warning(this, Text("Project"), Text("OpenFailed"));
		return;
	}
	projectPath = path;
	modified = false;
	subtitleUndo->clear();
	if (speechJob) {
		jobClip = -1;
		speechJob->Cancel();
	}
	SelectClip(-1);
	UpdateLibrary();
	UpdateTimeline();
}

bool EditorWindow::Save()
{
	QString path = projectPath;
	if (path.isEmpty())
		path = QFileDialog::getSaveFileName(this, Text("Save"), {}, Text("ProjectFilter"));
	if (path.isEmpty())
		return false;
	if (!path.endsWith(".obsedit"_L1, Qt::CaseInsensitive))
		path += ".obsedit"_L1;
	if (!project.Save(path)) {
		QMessageBox::warning(this, Text("Project"), Text("SaveFailed"));
		return false;
	}
	projectPath = path;
	modified = false;
	statusBar()->showMessage(Text("Saved"), 4000);
	return true;
}

void EditorWindow::Export()
{
	const QString parent = QFileDialog::getExistingDirectory(this, Text("ExportLocation"));
	if (parent.isEmpty())
		return;
	const QDir directory(parent);
	const QString name = projectPath.isEmpty() ? QString("OBS Video Editor"_L1) : QFileInfo(projectPath).completeBaseName();
	QString bundle = directory.filePath(name);
	for (int i = 2; QFileInfo::exists(bundle); ++i)
		bundle = directory.filePath(QStringLiteral("%1 (%2)").arg(name).arg(i));
	if (!project.Export(bundle)) {
		QMessageBox::warning(this, Text("Project"), Text("ExportFailed"));
		return;
	}
	statusBar()->showMessage(Text("Exported").arg(QDir::toNativeSeparators(bundle)), 10000);
}

void EditorWindow::UpdateLibrary()
{
	library->clear();
	for (const auto &asset : project.Assets()) {
		const auto name = QFileInfo(asset.path).fileName();
		auto *item = new QListWidgetItem(asset.enhanced ? Text("EnhancerMarked").arg(name) : name, library);
		item->setToolTip(QDir::toNativeSeparators(asset.path));
	}
}

void EditorWindow::UpdateTimeline()
{
	const QSignalBlocker blocker(timeline);
	timeline->clear();
	playhead = nullptr;
	constexpr int gutter = 100;
	constexpr int width = 840;
	const int duration = std::max(project.Duration(), 10000);
	const double scale = static_cast<double>(width) / duration;
	const int height = showSubtitles->isChecked() ? 157 : 116;
	timeline->setSceneRect(0, 0, gutter + width + 35, height);
	for (int tick = 0; tick <= 5; ++tick) {
		const int x = gutter + width * tick / 5;
		auto *label = timeline->addSimpleText(Text("Seconds").arg(duration / 1000.0 * tick / 5, 0, 'f', 1));
		label->setBrush(palette().text());
		label->setPos(x, 0);
		timeline->addLine(x, 20, x, height - 8, QPen(palette().mid(), 1));
	}
	for (Track track : {Track::Video, Track::Audio}) {
		const int y = track == Track::Video ? 28 : 72;
		auto *label = timeline->addSimpleText(Text(track == Track::Video ? "Video" : "Audio"));
		label->setBrush(palette().text());
		label->setPos(4, y + 6);
		timeline->addRect(gutter, y, width, 32, Qt::NoPen, palette().alternateBase());
	}
	for (qsizetype i = 0; i < project.Clips().size(); ++i) {
		const auto &clip = project.Clips()[i];
		const auto &asset = project.Assets()[clip.asset];
		const auto filename = QFileInfo(asset.path).fileName();
		const auto name = asset.enhanced ? Text("EnhancerMarked").arg(filename) : filename;
		const int y = asset.track == Track::Video ? 28 : 72;
		auto *block = timeline->addRect(0, 0, std::max(2.0, clip.Duration() * scale), 32,
						QPen(palette().mid(), 1), palette().highlight());
		block->setPos(gutter + clip.startMs * scale, y);
		block->setFlag(QGraphicsItem::ItemIsSelectable);
		block->setFlag(QGraphicsItem::ItemClipsChildrenToShape);
		block->setData(0, static_cast<int>(i));
		block->setToolTip(name + u'\n' + Text("Seconds").arg(clip.startMs / 1000.0, 0, 'f', 3));
		auto *label = new QGraphicsSimpleTextItem(name, block);
		label->setBrush(palette().highlightedText());
		label->setPos(5, 6);
		block->setSelected(i == selectedClip && selectedCue < 0);
	}
	if (showSubtitles->isChecked())
		AddSubtitleBlocks(scale, gutter);
	if (selectedClip >= 0) {
		playhead = timeline->addLine(0, 0, 0, 0, QPen(palette().text(), 1));
		playhead->setZValue(1);
	}
	UpdateSubtitlePreview(playheadSource);
}

void EditorWindow::SelectClip(int clip)
{
	selectedClip = clip;
	selectedCue = -1;
	previewTimer->stop();
	preview->Clear();
	seek->setValue(0);
	play->setText(Text("Play"));
	const bool selected = clip >= 0;
	enhance->setEnabled(selected && project.Assets()[project.Clips()[clip].asset].track == Track::Video);
	seek->setEnabled(selected);
	play->setEnabled(selected);
	move->setEnabled(selected);
	remove->setEnabled(selected);
	trim->setEnabled(selected);
	split->setEnabled(selected);
	inTime->setEnabled(selected);
	outTime->setEnabled(selected);
	startTime->setEnabled(selected);
	startTime->setValue(selected ? project.Clips()[clip].startMs / 1000.0 : 0);
	UpdateSubtitleActions();
	if (!selected) {
		details->setText(Text("NoSelection"));
		details->setToolTip({});
		inTime->setValue(0);
		outTime->setValue(0);
		UpdateSubtitlePreview(0);
		return;
	}
	const auto &segment = project.Clips()[clip];
	const auto &asset = project.Assets()[segment.asset];
	details->setText(Text("ClipDetails").arg(QFileInfo(asset.path).fileName()).arg(segment.Duration() / 1000.0, 0, 'f', 3));
	details->setToolTip(QDir::toNativeSeparators(asset.path));
	inTime->setMaximum(asset.durationMs / 1000.0);
	outTime->setMaximum(asset.durationMs / 1000.0);
	inTime->setValue(segment.inMs / 1000.0);
	outTime->setValue(segment.outMs / 1000.0);
	seek->setRange(0, segment.Duration() - 1);
	UpdateSubtitlePreview(segment.inMs);
}

void EditorWindow::UpdatePreviewPosition()
{
	if (!preview->Active() || selectedClip < 0)
		return;
	const auto &clip = project.Clips()[selectedClip];
	const int sourceTime = preview->Time();
	if (sourceTime >= clip.outMs || preview->Ended()) {
		preview->Clear();
		if (!seek->isSliderDown()) {
			const QSignalBlocker blocker(seek);
			seek->setValue(0);
		}
		UpdateSubtitlePreview(clip.outMs);
	} else {
		UpdateSubtitlePreview(sourceTime);
	}
	if (preview->Playing() && sourceTime >= clip.inMs && !seek->isSliderDown()) {
		const QSignalBlocker blocker(seek);
		seek->setValue(sourceTime - clip.inMs);
	}
	const bool playing = preview->Playing();
	play->setText(Text(playing ? "Pause" : "Play"));
	if (!playing && preview->Ended())
		previewTimer->stop();
}

void EditorWindow::Play()
{
	if (preview->Playing()) {
		preview->Pause();
		UpdatePreviewPosition();
		previewTimer->stop();
		return;
	}
	const auto &clip = project.Clips()[selectedClip];
	const auto &asset = project.Assets()[clip.asset];
	if (!QFileInfo(asset.path).isFile() || !preview->Play(asset.path, clip.inMs + seek->value())) {
		statusBar()->showMessage(Text("PreviewFailed"));
		return;
	}
	previewTimer->start();
	statusBar()->clearMessage();
}

void EditorWindow::dragEnterEvent(QDragEnterEvent *event)
{
	if (event->mimeData()->hasUrls() && event->possibleActions().testFlag(Qt::CopyAction)) {
		event->setDropAction(Qt::CopyAction);
		event->accept();
	}
}

void EditorWindow::dropEvent(QDropEvent *event)
{
	if (!event->mimeData()->hasUrls() || !event->possibleActions().testFlag(Qt::CopyAction))
		return;
	QStringList files;
	for (const auto &url : event->mimeData()->urls()) {
		if (url.isLocalFile())
			files.append(url.toLocalFile());
	}
	event->setDropAction(Qt::CopyAction);
	event->accept();
	ImportFiles(files);
}

void EditorWindow::hideEvent(QHideEvent *event)
{
	preview->Clear();
	previewTimer->stop();
	play->setText(Text("Play"));
	QMainWindow::hideEvent(event);
}
