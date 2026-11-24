#include "EditorWindow.hpp"
#include "EditorPreview.hpp"

#include <OBSApp.hpp>

#include <QAction>
#include <QDir>
#include <QDoubleSpinBox>
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
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;
using VideoEditor::Track;

EditorWindow::EditorWindow(QWidget *parent) : QMainWindow(parent, Qt::Window)
{
	setAttribute(Qt::WA_QuitOnClose, false);
	setWindowTitle("OBS Video Editor[*]"_L1);
	resize(1080, 680);

	auto *toolbar = addToolBar(Text("Project"));
	toolbar->setMovable(false);
	connect(toolbar->addAction(Text("Open")), &QAction::triggered, this, &EditorWindow::Open);
	connect(toolbar->addAction(Text("Import")), &QAction::triggered, this, &EditorWindow::Import);
	connect(toolbar->addAction(Text("Save")), &QAction::triggered, this, &EditorWindow::Save);
	connect(toolbar->addAction(Text("Export")), &QAction::triggered, this, &EditorWindow::Export);

	auto *workspace = new QSplitter(Qt::Vertical, this);
	setCentralWidget(workspace);
	auto *panes = new QSplitter(Qt::Horizontal, workspace);

	auto *mediaPanel = new QGroupBox(Text("Library"), panes);
	auto *mediaLayout = new QVBoxLayout(mediaPanel);
	library = new QListWidget(mediaPanel);
	append = new QPushButton(Text("Add"), mediaPanel);
	mediaLayout->addWidget(library);
	mediaLayout->addWidget(append);
	append->setEnabled(false);
	connect(library, &QListWidget::currentRowChanged, this, [this](int row) { append->setEnabled(row >= 0); });
	connect(append, &QPushButton::clicked, this, [this] {
		if (!project.Append(library->currentRow())) {
			statusBar()->showMessage(Text("InvalidPlacement"));
			return;
		}
		setWindowModified(true);
		SelectClip(static_cast<int>(project.Clips().size()) - 1);
		UpdateTimeline();
	});

	auto *previewPanel = new QGroupBox(Text("Preview"), panes);
	auto *previewLayout = new QVBoxLayout(previewPanel);
	preview = new EditorPreview(previewPanel);
	play = new QPushButton(Text("Play"), previewPanel);
	seek = new QSlider(Qt::Horizontal, previewPanel);
	seek->setAccessibleName(Text("Seek"));
	seek->setTracking(false);
	previewLayout->addWidget(preview, 1);
	previewLayout->addWidget(seek);
	previewLayout->addWidget(play);
	connect(play, &QPushButton::clicked, this, &EditorWindow::Play);
	connect(seek, &QSlider::valueChanged, preview, &EditorPreview::Seek);

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
	form->addRow(details);
	form->addRow(Text("StartSeconds"), startTime);
	form->addRow(move);
	form->addRow(remove);
	connect(move, &QPushButton::clicked, this, [this] {
		const int start = static_cast<int>(qRound64(startTime->value() * 1000));
		if (!project.Move(selectedClip, start)) {
			statusBar()->showMessage(Text("InvalidPlacement"));
			return;
		}
		statusBar()->clearMessage();
		setWindowModified(true);
		UpdateTimeline();
	});
	connect(remove, &QPushButton::clicked, this, [this] {
		project.Remove(selectedClip);
		setWindowModified(true);
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
		SelectClip(items.isEmpty() ? -1 : items.first()->data(0).toInt());
	});
	panes->setSizes({220, 600, 245});
	panes->setStretchFactor(1, 1);
	workspace->setSizes({470, 180});
	workspace->setStretchFactor(0, 1);

	auto *timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, [this] {
		if (!isVisible())
			return;
		if (!seek->isSliderDown()) {
			const QSignalBlocker blocker(seek);
			seek->setValue(preview->Time());
		}
		play->setText(Text(preview->Playing() ? "Pause" : "Play"));
	});
	timer->start(100);
	SelectClip(-1);
	UpdateTimeline();
}

EditorWindow::~EditorWindow()
{
	timeline->disconnect(this);
}

QString EditorWindow::Text(const char *key) const
{
	const QByteArray lookup = QByteArray("VideoEditor.") + key;
	return QTStr(lookup.constData());
}

void EditorWindow::Import()
{
	const auto files = QFileDialog::getOpenFileNames(this, Text("Import"));
	if (files.isEmpty())
		return;
	const auto previous = project.Assets().size();
	QStringList failed;
	for (const auto &file : files) {
		if (!project.Import(file))
			failed.append(QFileInfo(file).fileName());
	}
	if (previous != project.Assets().size()) {
		setWindowModified(true);
		UpdateLibrary();
		library->setCurrentRow(static_cast<int>(previous));
	}
	if (!failed.isEmpty())
		QMessageBox::warning(this, windowTitle(), Text("ImportFailed").arg(failed.join(u'\n')));
}

bool EditorWindow::ConfirmProjectClose()
{
	if (!isWindowModified())
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
	setWindowModified(false);
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
	setWindowModified(false);
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
		auto *item = new QListWidgetItem(QFileInfo(asset.path).fileName(), library);
		item->setToolTip(QDir::toNativeSeparators(asset.path));
	}
}

void EditorWindow::UpdateTimeline()
{
	const QSignalBlocker blocker(timeline);
	timeline->clear();
	constexpr int gutter = 100;
	constexpr int width = 840;
	const int duration = std::max(project.Duration(), 10000);
	const double scale = static_cast<double>(width) / duration;
	timeline->setSceneRect(0, 0, gutter + width + 35, 116);
	for (int tick = 0; tick <= 5; ++tick) {
		const int x = gutter + width * tick / 5;
		auto *label = timeline->addSimpleText(Text("Seconds").arg(duration / 1000.0 * tick / 5, 0, 'f', 1));
		label->setBrush(palette().text());
		label->setPos(x, 0);
		timeline->addLine(x, 20, x, 106, QPen(palette().mid(), 1));
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
		const auto name = QFileInfo(asset.path).fileName();
		const int y = asset.track == Track::Video ? 28 : 72;
		auto *block = timeline->addRect(0, 0, std::max(2.0, asset.durationMs * scale), 32,
						QPen(palette().mid(), 1), palette().highlight());
		block->setPos(gutter + clip.startMs * scale, y);
		block->setFlag(QGraphicsItem::ItemIsSelectable);
		block->setFlag(QGraphicsItem::ItemClipsChildrenToShape);
		block->setData(0, static_cast<int>(i));
		block->setToolTip(name + u'\n' + Text("Seconds").arg(clip.startMs / 1000.0, 0, 'f', 3));
		auto *label = new QGraphicsSimpleTextItem(name, block);
		label->setBrush(palette().highlightedText());
		label->setPos(5, 6);
		block->setSelected(i == selectedClip);
	}
}

void EditorWindow::SelectClip(int clip)
{
	selectedClip = clip;
	preview->Clear();
	seek->setValue(0);
	seek->setEnabled(false);
	play->setText(Text("Play"));
	const bool selected = clip >= 0;
	play->setEnabled(selected);
	move->setEnabled(selected);
	remove->setEnabled(selected);
	startTime->setEnabled(selected);
	startTime->setValue(selected ? project.Clips()[clip].startMs / 1000.0 : 0);
	if (!selected) {
		details->setText(Text("NoSelection"));
		return;
	}
	const auto &asset = project.Assets()[project.Clips()[clip].asset];
	details->setText(Text("ClipDetails").arg(QFileInfo(asset.path).fileName()).arg(asset.durationMs / 1000.0, 0, 'f', 3));
	details->setToolTip(QDir::toNativeSeparators(asset.path));
	seek->setRange(0, asset.durationMs - 1);
}

void EditorWindow::Play()
{
	const auto &asset = project.Assets()[project.Clips()[selectedClip].asset];
	if (!QFileInfo(asset.path).isFile() || !preview->Play(asset.path, true)) {
		statusBar()->showMessage(Text("PreviewFailed"));
		return;
	}
	seek->setEnabled(true);
	statusBar()->clearMessage();
}

void EditorWindow::hideEvent(QHideEvent *event)
{
	preview->Clear();
	seek->setEnabled(false);
	QMainWindow::hideEvent(event);
}
