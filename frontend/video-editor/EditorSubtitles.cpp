#include "EditorWindow.hpp"
#include "EditorPreview.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QProgressDialog>
#include <QPushButton>
#include <QSlider>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

using namespace Qt::StringLiterals;
using VideoEditor::Subtitle;

namespace {
class SubtitleChange : public QUndoCommand {
	std::function<void(const QVector<Subtitle> &, int)> apply;
	QVector<Subtitle> before;
	QVector<Subtitle> after;
	int oldSelection;
	int newSelection;

public:
	SubtitleChange(std::function<void(const QVector<Subtitle> &, int)> apply, QVector<Subtitle> before,
		       QVector<Subtitle> after, int oldSelection, int newSelection)
		: apply(std::move(apply)),
		  before(std::move(before)),
		  after(std::move(after)),
		  oldSelection(oldSelection),
		  newSelection(newSelection)
	{
	}
	void undo() override { apply(before, oldSelection); }
	void redo() override { apply(after, newSelection); }
};

class SubtitleBlock : public QGraphicsRectItem {
	std::function<void(int, double)> change;
	double start = 0;
	QRectF initialRect;
	QPointF initialPosition;
	int mode = 0;

public:
	explicit SubtitleBlock(std::function<void(int, double)> change) : change(std::move(change))
	{
		setFlag(ItemIsSelectable);
		setFlag(ItemClipsChildrenToShape);
		setAcceptedMouseButtons(Qt::LeftButton);
	}

protected:
	void mousePressEvent(QGraphicsSceneMouseEvent *event) override
	{
		start = event->scenePos().x();
		initialRect = rect();
		initialPosition = pos();
		const double handle = std::min(6.0, rect().width() / 4);
		mode = event->pos().x() < handle ? -1 : (event->pos().x() >= rect().width() - handle ? 1 : 0);
		QGraphicsRectItem::mousePressEvent(event);
	}
	void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override
	{
		const double delta = event->scenePos().x() - start;
		if (mode < 0) {
			const auto shift = std::min(delta, initialRect.width() - 2);
			setX(initialPosition.x() + shift);
			setRect(0, 0, initialRect.width() - shift, initialRect.height());
		} else if (mode > 0) {
			setRect(0, 0, std::max(2.0, initialRect.width() + delta), initialRect.height());
		} else {
			setX(initialPosition.x() + delta);
		}
		event->accept();
	}
	void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override
	{
		QGraphicsRectItem::mouseReleaseEvent(event);
		setRect(initialRect);
		setPos(initialPosition);
		const double delta = event->scenePos().x() - start;
		if (std::abs(delta) >= 2)
			change(mode, delta);
	}
};
}

void EditorWindow::SetupSubtitles()
{
	subtitleUndo = new QUndoStack(this);
	subtitleUndo->setUndoLimit(50);
	addToolBarBreak();
	auto *subtitleTools = addToolBar(Text("Subtitles"));
	subtitleTools->setMovable(false);
	generate = subtitleTools->addAction(Text("GenerateSubtitles"), this, &EditorWindow::GenerateSubtitles);
	addSubtitle = subtitleTools->addAction(Text("NewSubtitle"), this, [this] { EditSubtitle(true); });
	cueActions.append(subtitleTools->addAction(Text("EditSubtitle"), this, [this] { EditSubtitle(false); }));
	cueActions.append(subtitleTools->addAction(Text("EditWord"), this, &EditorWindow::EditSubtitleWord));
	auto *more = new QMenu(Text("SubtitleActions"), this);
	auto *menuButton = new QPushButton(Text("SubtitleActions"), subtitleTools);
	menuButton->setMenu(more);
	subtitleTools->addWidget(menuButton);
	cueActions.append(more->addAction(Text("SplitSubtitle"), this, &EditorWindow::SplitSubtitle));
	cueActions.append(more->addAction(Text("MergeSubtitle"), this, &EditorWindow::MergeSubtitle));
	cueActions.append(more->addAction(Text("CopySubtitle"), this, &EditorWindow::CopySubtitle));
	pasteSubtitle = more->addAction(Text("PasteSubtitle"), this, &EditorWindow::PasteSubtitle);
	cueActions.append(more->addAction(Text("DeleteSubtitle"), this, [this] {
		auto cues = project.Clips()[selectedClip].subtitles;
		cues.removeAt(selectedCue);
		ChangeSubtitles(selectedClip, std::move(cues), -1);
	}));
	more->addAction(subtitleUndo->createUndoAction(more, Text("SubtitleUndo")));
	more->addAction(subtitleUndo->createRedoAction(more, Text("SubtitleRedo")));
	more->addAction(Text("ExportSrt"), this, &EditorWindow::ExportSubtitles);
	showSubtitles = more->addAction(Text("ShowSubtitles"));
	showSubtitles->setCheckable(true);
	showSubtitles->setChecked(true);
	connect(showSubtitles, &QAction::toggled, this, [this](bool visible) {
		if (!visible)
			selectedCue = -1;
		UpdateTimeline();
		UpdateSubtitleActions();
		UpdateSubtitlePreview(playheadSource);
	});
}

void EditorWindow::UpdateSubtitleActions()
{
	generate->setEnabled(selectedClip >= 0 && !speechJob);
	addSubtitle->setEnabled(selectedClip >= 0);
	pasteSubtitle->setEnabled(selectedClip >= 0 && !copiedCue.text.isEmpty());
	for (auto *action : cueActions)
		action->setEnabled(selectedCue >= 0);
}

void EditorWindow::AddSubtitleBlocks(double scale, int gutter)
{
	constexpr int row = 116;
	auto *heading = timeline->addSimpleText(Text("Subtitles"));
	heading->setBrush(palette().text());
	heading->setPos(4, row + 6);
	timeline->addRect(gutter, row, 840, 32, Qt::NoPen, palette().alternateBase());
	for (qsizetype i = 0; i < project.Clips().size(); ++i) {
		const auto &clip = project.Clips()[i];
		for (qsizetype j = 0; j < clip.subtitles.size(); ++j) {
			const auto &cue = clip.subtitles[j];
			const int start = std::max(cue.inMs, clip.inMs);
			const int end = std::min(cue.outMs, clip.outMs);
			if (end <= start)
				continue;
			auto *block = new SubtitleBlock([this, i, j, scale](int mode, double delta) {
				QTimer::singleShot(0, this, [this, i, j, mode, delta, scale] {
					const double milliseconds = std::clamp(delta / scale,
						double(std::numeric_limits<int>::min()), double(std::numeric_limits<int>::max()));
					DragSubtitle(static_cast<int>(i), static_cast<int>(j), mode, qRound(milliseconds));
				});
			});
			timeline->addItem(block);
			block->setRect(0, 0, std::max(2.0, (end - start) * scale), 32);
			block->setPos(gutter + (clip.startMs + (start - clip.inMs)) * scale, row);
			block->setPen(QPen(palette().mid(), 1));
			block->setBrush(palette().highlight());
			block->setData(0, static_cast<int>(i));
			block->setData(1, static_cast<int>(j));
			block->setToolTip(u"<qt>"_s + cue.text.toHtmlEscaped() + u"</qt>"_s);
			block->setSelected(i == selectedClip && j == selectedCue);
			auto *label = new QGraphicsSimpleTextItem(cue.text, block);
			label->setPos(5, 6);
			label->setBrush(palette().highlightedText());
			label->setAcceptedMouseButtons(Qt::NoButton);
		}
	}
}

void EditorWindow::UpdateSubtitlePreview(int sourceMs)
{
	playheadSource = sourceMs;
	QStringList lines;
	if (selectedClip >= 0) {
		const auto &clip = project.Clips()[selectedClip];
		if (showSubtitles->isChecked() && sourceMs >= clip.inMs && sourceMs < clip.outMs) {
			for (const auto &cue : clip.subtitles) {
				if (cue.inMs <= sourceMs && sourceMs < cue.outMs)
					lines.append(cue.text);
			}
		}
		if (playhead) {
			const auto scale = 840.0 / std::max(project.Duration(), 10000);
			const auto x = 100 + (clip.startMs + (std::clamp(sourceMs, clip.inMs, clip.outMs) - clip.inMs)) * scale;
			playhead->setLine(x, 20, x, showSubtitles->isChecked() ? 150 : 105);
		}
	}
	preview->SetSubtitle(lines.join(u'\n'));
}

void EditorWindow::ChangeSubtitles(int clip, QVector<Subtitle> subtitles, int selection)
{
	const int previous = selectedClip == clip ? selectedCue : -1;
	subtitleUndo->push(new SubtitleChange(
		[this, clip](const QVector<Subtitle> &cues, int row) {
			if (!project.SetSubtitles(clip, cues))
				return;
			modified = true;
			if (selectedClip == clip)
				selectedCue = row;
			UpdateTimeline();
			UpdateSubtitleActions();
		},
		project.Clips()[clip].subtitles, std::move(subtitles), previous, selection));
}

void EditorWindow::GenerateSubtitles()
{
	if (selectedClip < 0 || speechJob)
		return;
	const int target = selectedClip;
	const auto clip = project.Clips()[target];
	const auto media = project.Assets()[clip.asset].path;
	QDialog dialog(this);
	dialog.setWindowTitle(Text("GenerateSubtitles"));
	QFormLayout form(&dialog);
	const auto fileField = [&](const QString &label, const QString &initial) {
		auto *row = new QWidget(&dialog);
		auto *layout = new QHBoxLayout(row);
		layout->setContentsMargins(0, 0, 0, 0);
		auto *field = new QLineEdit(initial, row);
		auto *browse = new QPushButton(Text("SubtitleBrowse"), row);
		layout->addWidget(field, 1);
		layout->addWidget(browse);
		form.addRow(label, row);
		connect(browse, &QPushButton::clicked, &dialog, [field, label, &dialog] {
			const auto path = QFileDialog::getOpenFileName(&dialog, label, field->text());
			if (!path.isEmpty())
				field->setText(path);
		});
		return field;
	};
	auto *ffmpeg = fileField(u"FFmpeg"_s, speechOptions.ffmpeg.isEmpty() ? QStandardPaths::findExecutable(u"ffmpeg"_s)
									    : speechOptions.ffmpeg);
	auto *whisper = fileField(u"whisper-cli"_s, speechOptions.whisper.isEmpty()
						      ? QStandardPaths::findExecutable(u"whisper-cli"_s)
						      : speechOptions.whisper);
	auto *model = fileField(Text("SubtitleModel"), speechOptions.model);
	QComboBox language(&dialog);
	language.addItem(Text("SubtitleAuto"), u"auto"_s);
	language.addItem(Text("SubtitlePortuguese"), u"pt"_s);
	language.addItem(Text("SubtitleEnglish"), u"en"_s);
	language.addItem(Text("SubtitleSpanish"), u"es"_s);
	language.setCurrentIndex(std::max(0, language.findData(speechOptions.language)));
	form.addRow(Text("SubtitleLanguage"), &language);
	QCheckBox cpu(Text("SubtitleCpu"), &dialog);
	cpu.setChecked(speechOptions.cpu);
	form.addRow(&cpu);
	QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	form.addRow(&buttons);
	connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	if (dialog.exec() != QDialog::Accepted)
		return;
	speechOptions = {QFileInfo(ffmpeg->text()).canonicalFilePath(), QFileInfo(whisper->text()).canonicalFilePath(),
			 QFileInfo(model->text()).canonicalFilePath(), language.currentData().toString(), cpu.isChecked()};
	if (!QFileInfo(speechOptions.ffmpeg).isExecutable() || !QFileInfo(speechOptions.whisper).isExecutable() ||
	    !QFileInfo(speechOptions.model).isFile()) {
		QMessageBox::warning(this, Text("Subtitles"), Text("SubtitleInvalidTools"));
		return;
	}
	auto *progress = new QProgressDialog(Text("SubtitleExtracting"), Text("SubtitleCancel"), 0, 100, this);
	progress->setWindowTitle(Text("GenerateSubtitles"));
	progress->setMinimumDuration(0);
	progress->setAutoClose(false);
	progress->setAutoReset(false);
	progress->setValue(0);
	progress->show();
	jobClip = target;
	auto *job = new SpeechRecognition(speechOptions, media, clip.inMs, clip.outMs, this);
	speechJob = job;
	connect(progress, &QProgressDialog::canceled, job, &SpeechRecognition::Cancel);
	job->progress = [this, progress](int value, bool recognizing) {
		progress->setLabelText(Text(recognizing ? "SubtitleProcessing" : "SubtitleExtracting").arg(value));
		progress->setValue(value);
	};
	job->completed = [this, job, progress](QVector<Subtitle> result, QString error, bool canceled) {
		const int target = std::exchange(jobClip, -1);
		speechJob = nullptr;
		progress->hide();
		progress->deleteLater();
		job->deleteLater();
		UpdateSubtitleActions();
		if (canceled || target < 0)
			return;
		if (result.isEmpty()) {
			const QString message = error.isNull() ? Text("SubtitleNoSpeech") : Text("SubtitleFailed").arg(error);
			QMessageBox box(QMessageBox::Warning, Text("Subtitles"), {}, QMessageBox::Ok, this);
			box.setTextFormat(Qt::PlainText);
			box.setText(message);
			box.exec();
			return;
		}
		auto cues = project.Clips()[target].subtitles;
		const auto existing = cues;
		for (auto &cue : result) {
			if (std::none_of(existing.begin(), existing.end(), [&](const Subtitle &previous) {
				    return cue.inMs < previous.outMs && previous.inMs < cue.outMs;
			    }))
				cues.append(std::move(cue));
		}
		if (cues.size() == existing.size()) {
			statusBar()->showMessage(Text("SubtitleAlreadyPresent"), 5000);
			return;
		}
		ChangeSubtitles(target, std::move(cues), selectedClip == target ? selectedCue : -1);
	};
	UpdateSubtitleActions();
	job->Start();
}

void EditorWindow::AdjustSubtitleJob(int clip, int delta)
{
	if (jobClip == clip) {
		jobClip = -1;
		speechJob->Cancel();
	} else if (jobClip > clip) {
		jobClip += delta;
	}
}

void EditorWindow::EditSubtitle(bool add)
{
	if (selectedClip < 0 || (!add && selectedCue < 0))
		return;
	const int target = selectedClip;
	const int editing = selectedCue;
	const auto clip = project.Clips()[target];
	const int position = clip.inMs + seek->value();
	Subtitle cue = add ? Subtitle{position, position + std::min(2000, clip.outMs - position), {}, {}, {}}
			   : clip.subtitles[editing];
	QDialog dialog(this);
	dialog.setWindowTitle(Text(add ? "NewSubtitle" : "EditSubtitle"));
	QFormLayout form(&dialog);
	QLineEdit text(cue.text, &dialog);
	form.addRow(Text("SubtitleText"), &text);
	QDoubleSpinBox start(&dialog);
	QDoubleSpinBox end(&dialog);
	for (auto *spin : {&start, &end}) {
		spin->setDecimals(3);
		spin->setRange(clip.startMs / 1000.0, clip.End() / 1000.0);
	}
	start.setValue((clip.startMs + (std::max(cue.inMs, clip.inMs) - clip.inMs)) / 1000.0);
	end.setValue((clip.startMs + (std::min(cue.outMs, clip.outMs) - clip.inMs)) / 1000.0);
	form.addRow(Text("StartSeconds"), &start);
	form.addRow(Text("SubtitleEnd"), &end);
	QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	form.addRow(&buttons);
	connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	connect(&buttons, &QDialogButtonBox::accepted, &dialog, [&] {
		if (!text.text().trimmed().isEmpty() && start.value() < end.value())
			dialog.accept();
		else
			QMessageBox::warning(&dialog, Text("Subtitles"), Text("SubtitleInvalidCue"));
	});
	if (dialog.exec() != QDialog::Accepted)
		return;
	const int in = clip.inMs + (qRound(start.value() * 1000) - clip.startMs);
	const int out = clip.inMs + (qRound(end.value() * 1000) - clip.startMs);
	if (cue.text != text.text().simplified())
		cue.words.clear();
	cue.text = text.text().simplified();
	cue.inMs = in;
	cue.outMs = out;
	auto cues = project.Clips()[target].subtitles;
	const int row = add ? static_cast<int>(cues.size()) : editing;
	if (add)
		cues.append(std::move(cue));
	else
		cues[row] = std::move(cue);
	ChangeSubtitles(target, std::move(cues), row);
}

void EditorWindow::EditSubtitleWord()
{
	const int target = selectedClip;
	const int row = selectedCue;
	auto cue = project.Clips()[target].subtitles[row];
	auto words = cue.text.simplified().split(u' ');
	QStringList choices;
	for (qsizetype i = 0; i < words.size(); ++i)
		choices.append(QStringLiteral("%1 · %2").arg(i + 1).arg(words[i]));
	bool ok;
	const auto choice = QInputDialog::getItem(this, Text("EditWord"), Text("SubtitleChooseWord"), choices, 0, false, &ok);
	if (!ok)
		return;
	const auto index = choices.indexOf(choice);
	if (index < 0)
		return;
	auto replacement = QInputDialog::getText(this, Text("EditWord"), Text("SubtitleText"), QLineEdit::Normal,
						  words[index], &ok).simplified();
	if (!ok || replacement.isEmpty())
		return;
	if (replacement.contains(u' '))
		cue.words.clear();
	words[index] = replacement;
	cue.text = words.join(u' ');
	auto cues = project.Clips()[target].subtitles;
	cues[row] = std::move(cue);
	ChangeSubtitles(target, std::move(cues), row);
}

void EditorWindow::SplitSubtitle()
{
	const int target = selectedClip;
	const int row = selectedCue;
	const auto &clip = project.Clips()[target];
	auto left = clip.subtitles[row];
	const int time = clip.inMs + seek->value();
	const auto words = left.text.simplified().split(u' ');
	if (time <= left.inMs || time >= left.outMs || words.size() < 2) {
		statusBar()->showMessage(Text("SubtitleInvalidSplit"), 5000);
		return;
	}
	int suggested = static_cast<int>(words.size() / 2);
	if (left.words.size() == words.size())
		suggested = static_cast<int>(std::count_if(left.words.begin(), left.words.end(),
							  [time](const auto &word) { return word.inMs < time; }));
	bool ok;
	const int maximum = static_cast<int>(words.size()) - 1;
	const int count = QInputDialog::getInt(this, Text("SplitSubtitle"), Text("SubtitleSplitWord"),
					       std::clamp(suggested, 1, maximum), 1, maximum, 1, &ok);
	if (!ok)
		return;
	auto right = left;
	left.outMs = time;
	right.inMs = time;
	left.text = words.mid(0, count).join(u' ');
	right.text = words.mid(count).join(u' ');
	if (left.words.size() == words.size()) {
		right.words = left.words.mid(count);
		left.words.resize(count);
	}
	auto cues = project.Clips()[target].subtitles;
	cues[row] = std::move(left);
	cues.insert(row + 1, std::move(right));
	ChangeSubtitles(target, std::move(cues), row + 1);
}

void EditorWindow::MergeSubtitle()
{
	auto cues = project.Clips()[selectedClip].subtitles;
	auto &cue = cues[selectedCue];
	int next = -1;
	for (qsizetype i = 0; i < cues.size(); ++i) {
		if (i != selectedCue && cues[i].inMs >= cue.inMs && (next < 0 || cues[i].inMs < cues[next].inMs))
			next = static_cast<int>(i);
	}
	if (next < 0) {
		statusBar()->showMessage(Text("SubtitleNoNext"), 5000);
		return;
	}
	const auto right = cues[next];
	cue.outMs = std::max(cue.outMs, right.outMs);
	if (cue.words.isEmpty() || right.words.isEmpty())
		cue.words.clear();
	else
		cue.words += right.words;
	if (cue.language != right.language)
		cue.language.clear();
	cue.text += u' ' + right.text;
	cues.removeAt(next);
	ChangeSubtitles(selectedClip, std::move(cues), selectedCue - (next < selectedCue ? 1 : 0));
}

void EditorWindow::CopySubtitle()
{
	copiedCue = project.Clips()[selectedClip].subtitles[selectedCue];
	UpdateSubtitleActions();
}

void EditorWindow::PasteSubtitle()
{
	if (selectedClip < 0 || copiedCue.text.isEmpty())
		return;
	const auto &clip = project.Clips()[selectedClip];
	auto cues = clip.subtitles;
	auto cue = copiedCue;
	const int position = clip.inMs + seek->value();
	cue.outMs = position + std::min(cue.outMs - cue.inMs, clip.outMs - position);
	cue.inMs = position;
	cue.words.clear();
	cues.append(std::move(cue));
	const int row = static_cast<int>(cues.size()) - 1;
	ChangeSubtitles(selectedClip, std::move(cues), row);
}

void EditorWindow::DragSubtitle(int clipIndex, int index, int mode, int delta)
{
	if (clipIndex >= project.Clips().size() || index >= project.Clips()[clipIndex].subtitles.size())
		return;
	const auto &clip = project.Clips()[clipIndex];
	auto cues = clip.subtitles;
	auto &cue = cues[index];
	const int start = std::max(cue.inMs, clip.inMs);
	const int end = std::min(cue.outMs, clip.outMs);
	cue.inMs = start;
	cue.outMs = end;
	if (mode < 0)
		cue.inMs = start + std::clamp(delta, clip.inMs - start, end - start - 1);
	else if (mode > 0)
		cue.outMs = end + std::clamp(delta, start - end + 1, clip.outMs - end);
	else {
		delta = std::clamp(delta, clip.inMs - start, clip.outMs - end);
		cue.inMs += delta;
		cue.outMs += delta;
		for (auto &word : cue.words) {
			const int assetEnd = project.Assets()[clip.asset].durationMs;
			const int shift = std::clamp(delta, -word.inMs, assetEnd - word.outMs);
			word.inMs += shift;
			word.outMs += shift;
		}
	}
	ChangeSubtitles(clipIndex, std::move(cues), index);
}

void EditorWindow::ExportSubtitles()
{
	QString path = QFileDialog::getSaveFileName(this, Text("ExportSrt"), {}, u"SubRip (*.srt)"_s);
	if (path.isEmpty())
		return;
	if (!path.endsWith(u".srt"_s, Qt::CaseInsensitive))
		path += u".srt"_s;
	if (!project.ExportSrt(path))
		QMessageBox::warning(this, Text("Subtitles"), Text("SubtitleExportFailed"));
}
