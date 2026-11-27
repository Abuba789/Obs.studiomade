#include "EditorWindow.hpp"
#include "EditorPreview.hpp"

#include <OBSApp.hpp>

#include <QAction>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QInputDialog>
#include <QJsonDocument>
#include <QMap>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QProgressDialog>
#include <QScrollArea>
#include <QSettings>
#include <QPushButton>
#include <QSpinBox>
#include <QSlider>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

using namespace Qt::StringLiterals;
using VideoEditor::Subtitle;

namespace {
VideoEditor::Caption CaptionAt(const Subtitle &cue, int time, int start, int end)
{
	VideoEditor::Caption caption{cue.text, cue.style, -1, 100, 0};
	if (cue.style["highlight"_L1].toBool()) {
		for (qsizetype i = 0; i < cue.words.size(); ++i) {
			const auto &word = cue.words[i];
			if (word.inMs <= time && time < word.outMs) {
				caption.word = static_cast<int>(i);
				break;
			}
		}
	}
	const auto strength = [](int elapsed, int duration) {
		return duration > 0 ? static_cast<int>(100LL * std::clamp(elapsed, 0, duration) / duration) : 100;
	};
	const int half = (end - start) / 2;
	caption.opacity = std::min(strength(time - start, std::min(half, cue.style["fadeIn"_L1].toInt())),
				   strength(end - time, std::min(half, cue.style["fadeOut"_L1].toInt())));
	if (cue.style["slide"_L1].toBool())
		caption.offset = (100 - strength(time - start, std::min(200, end - start))) / 5;
	return caption;
}

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
	std::function<void()> edit;
	double start = 0;
	QRectF initialRect;
	QPointF initialPosition;
	int mode = 0;

public:
	SubtitleBlock(std::function<void(int, double)> change, std::function<void()> edit)
		: change(std::move(change)), edit(std::move(edit))
	{
		setFlag(ItemIsSelectable);
		setFlag(ItemClipsChildrenToShape);
		setAcceptedMouseButtons(Qt::LeftButton);
	}

protected:
	void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override
	{
		event->accept();
		edit();
	}

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
	addSubtitle = subtitleTools->addAction(Text("NewSubtitle"), this, [this] { EditSubtitle(true); });
	cueActions.append(subtitleTools->addAction(Text("EditSubtitle"), this, [this] { EditSubtitle(false); }));
	cueActions.append(subtitleTools->addAction(Text("EditWord"), this, &EditorWindow::EditSubtitleWord));
	cueActions.append(subtitleTools->addAction(Text("SubtitleStyle"), this, &EditorWindow::EditSubtitleStyle));
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
	more->addAction(Text("ExportSubtitleFile"), this, &EditorWindow::ExportSubtitles);
	more->addAction(Text("ExportCaptionVideo"), this, &EditorWindow::ExportCaptionVideo);
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
			}, [this, i, j] {
				QTimer::singleShot(0, this, [this, i, j] {
					if (selectedClip == i && selectedCue == j)
						EditSubtitle(false);
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
	QVector<VideoEditor::Caption> captions;
	if (selectedClip >= 0) {
		const auto &clip = project.Clips()[selectedClip];
		if (showSubtitles->isChecked() && sourceMs >= clip.inMs && sourceMs < clip.outMs) {
			const int position = clip.startMs + (sourceMs - clip.inMs);
			for (qsizetype i = 0; i < project.Clips().size(); ++i) {
				const auto &segment = project.Clips()[i];
				if (position < segment.startMs || position >= segment.End())
					continue;
				const int time = segment.inMs + (position - segment.startMs);
				for (qsizetype j = 0; j < segment.subtitles.size(); ++j) {
					const auto &cue = segment.subtitles[j];
					if (time < cue.inMs || time >= cue.outMs)
						continue;
					auto caption = CaptionAt(cue, time, std::max(cue.inMs, segment.inMs),
								 std::min(cue.outMs, segment.outMs));
					if (i == selectedClip && j == selectedCue && !preview->Playing()) {
						caption.opacity = 100;
						caption.offset = 0;
					}
					captions.append(std::move(caption));
				}
			}
		}
		if (playhead) {
			const auto scale = 840.0 / std::max(project.Duration(), 10000);
			const auto x = 100 + (clip.startMs + (std::clamp(sourceMs, clip.inMs, clip.outMs) - clip.inMs)) * scale;
			playhead->setLine(x, 20, x, showSubtitles->isChecked() ? 150 : 105);
		}
	}
	preview->SetSubtitles(captions);
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


void EditorWindow::EditSubtitleStyle()
{
	const int target = selectedClip;
	const int row = selectedCue;
	const auto cue = project.Clips()[target].subtitles[row];
	QDialog dialog(this);
	dialog.setWindowTitle(Text("SubtitleStyle"));
	auto *layout = new QVBoxLayout(&dialog);
	auto *scroll = new QScrollArea(&dialog);
	auto *panel = new QWidget(scroll);
	QFormLayout form(panel);
	scroll->setWidget(panel);
	scroll->setWidgetResizable(true);
	layout->addWidget(scroll);
	QComboBox presets(&dialog);
	presets.addItems({Text("SubtitleMovie"), Text("SubtitleDynamic"), Text("SubtitleShort"), Text("SubtitleMinimal")});
	form.addRow(Text("SubtitlePreset"), &presets);
	QFontComboBox font(&dialog);
	form.addRow(Text("SubtitleFont"), &font);
	QMap<QString, QSpinBox *> numbers;
	const auto number = [&](const char *label, const QString &key, int minimum, int maximum, int initial) {
		auto *spin = new QSpinBox(&dialog);
		spin->setRange(minimum, maximum);
		spin->setValue(initial);
		numbers.insert(key, spin);
		spin->setProperty("initial", initial);
		form.addRow(Text(label), spin);
	};
	number("SubtitleSize", u"size"_s, 8, 100, 23);
	number("SubtitleOutline", u"outline"_s, 0, 10, 0);
	number("SubtitleShadow", u"shadow"_s, 0, 10, 0);
	number("SubtitleBackground", u"background"_s, 0, 255, 180);
	number("SubtitleSpacing", u"spacing"_s, -5, 20, 0);
	number("SubtitleX", u"x"_s, 0, 100, 50);
	number("SubtitleY", u"y"_s, 0, 100, 90);
	number("SubtitleFadeIn", u"fadeIn"_s, 0, 5000, 0);
	number("SubtitleFadeOut", u"fadeOut"_s, 0, 5000, 0);
	QMap<QString, QCheckBox *> checks;
	const auto check = [&](const char *label, const QString &key) {
		auto *box = new QCheckBox(Text(label), &dialog);
		checks.insert(key, box);
		form.addRow(box);
	};
	check("SubtitleBold", u"bold"_s);
	check("SubtitleItalic", u"italic"_s);
	check("SubtitleHighlight", u"highlight"_s);
	check("SubtitleSlide", u"slide"_s);
	QComboBox alignment(&dialog);
	alignment.addItems({Text("SubtitleLeft"), Text("SubtitleCenter"), Text("SubtitleRight")});
	form.addRow(Text("SubtitleAlignment"), &alignment);
	QComboBox position(&dialog);
	position.addItems({Text("SubtitleTop"), Text("SubtitleCenter"), Text("SubtitleBottom"), Text("SubtitleCustom")});
	form.addRow(Text("SubtitlePosition"), &position);
	QPushButton foreground(&dialog);
	QPushButton highlight(&dialog);
	form.addRow(Text("SubtitleColor"), &foreground);
	form.addRow(Text("SubtitleHighlightColor"), &highlight);
	QCheckBox all(Text("SubtitleApplyClip"), &dialog);
	form.addRow(&all);
	BPtr<char> presetPath = GetAppConfigPathPtr("obs-studio/editor-subtitle-styles.ini");
	QSettings settings(QString::fromUtf8(presetPath.Get()), QSettings::IniFormat);
	settings.beginGroup(u"SubtitleStyles"_s);
	QComboBox saved(&dialog);
	saved.addItems(settings.childKeys());
	QPushButton saveStyle(Text("SubtitleSaveStyle"), &dialog);
	form.addRow(&saved, &saveStyle);
	const auto setColor = [](QPushButton &button, const QColor &color) {
		button.setText(color.name());
	};
	const auto style = [&] {
		QJsonObject value{{"font"_L1, font.currentFont().family()}, {"align"_L1, alignment.currentIndex()},
				  {"position"_L1, position.currentIndex()}, {"color"_L1, foreground.text()},
				  {"highlightColor"_L1, highlight.text()}};
		for (auto it = numbers.cbegin(); it != numbers.cend(); ++it)
			value.insert(it.key(), it.value()->value());
		for (auto it = checks.cbegin(); it != checks.cend(); ++it)
			value.insert(it.key(), it.value()->isChecked());
		return value;
	};
	const auto refresh = [&] {
		auto sample = cue;
		sample.style = style();
		auto caption = CaptionAt(sample, sample.inMs + (sample.outMs - sample.inMs) / 2, sample.inMs, sample.outMs);
		caption.opacity = 100;
		caption.offset = 0;
		if (sample.style["highlight"_L1].toBool() && !sample.words.isEmpty() && caption.word < 0)
			caption.word = 0;
		preview->SetSubtitles({caption});
	};
	const auto load = [&](const QJsonObject &value) {
		font.setCurrentFont(QFont(value["font"_L1].toString()));
		for (auto it = numbers.cbegin(); it != numbers.cend(); ++it)
			it.value()->setValue(value[it.key()].toInt(it.value()->property("initial").toInt()));
		for (auto it = checks.cbegin(); it != checks.cend(); ++it)
			it.value()->setChecked(value[it.key()].toBool(it.key() == "bold"_L1));
		alignment.setCurrentIndex(std::clamp(value["align"_L1].toInt(1), 0, 2));
		position.setCurrentIndex(std::clamp(value["position"_L1].toInt(2), 0, 3));
		setColor(foreground, QColor(value["color"_L1].toString(u"#ffffff"_s)));
		setColor(highlight, QColor(value["highlightColor"_L1].toString(u"#ffe34d"_s)));
		refresh();
	};
	const QVector<QJsonObject> styles{
		{{"size"_L1, 23}, {"outline"_L1, 2}, {"shadow"_L1, 1}, {"background"_L1, 0}},
		{{"size"_L1, 29}, {"outline"_L1, 3}, {"highlight"_L1, true}, {"background"_L1, 0}},
		{{"size"_L1, 39}, {"color"_L1, "#65eaff"_L1}, {"outline"_L1, 4}, {"highlight"_L1, true},
		 {"background"_L1, 0}, {"fadeIn"_L1, 160}, {"fadeOut"_L1, 160}, {"slide"_L1, true}},
		{{"size"_L1, 18}, {"bold"_L1, false}, {"background"_L1, 95}},
	};
	connect(&presets, &QComboBox::activated, &dialog, [&](int index) { load(styles[index]); });
	connect(&saved, &QComboBox::activated, &dialog, [&](int index) {
		load(QJsonDocument::fromJson(settings.value(saved.itemText(index)).toByteArray()).object());
	});
	connect(&saveStyle, &QPushButton::clicked, &dialog, [&] {
		bool ok;
		const auto name = QInputDialog::getText(&dialog, Text("SubtitleSaveStyle"), Text("SubtitlePreset"),
						      QLineEdit::Normal, {}, &ok).trimmed();
		if (!ok || name.isEmpty() || name.contains(u'/') || name.contains(u'\\'))
			return;
		settings.setValue(name, QJsonDocument(style()).toJson(QJsonDocument::Compact));
		settings.sync();
		if (settings.status() != QSettings::NoError) {
			QMessageBox::warning(&dialog, Text("SubtitleSaveStyle"), Text("SubtitleStyleSaveFailed"));
			return;
		}
		if (saved.findText(name) < 0)
			saved.addItem(name);
		saved.setCurrentText(name);
	});
	for (auto *spin : numbers)
		connect(spin, &QSpinBox::valueChanged, &dialog, refresh);
	for (auto *box : checks)
		connect(box, &QCheckBox::toggled, &dialog, refresh);
	for (auto *combo : {&alignment, &position})
		connect(combo, &QComboBox::currentIndexChanged, &dialog, refresh);
	connect(&font, &QFontComboBox::currentFontChanged, &dialog, refresh);
	for (auto *button : {&foreground, &highlight}) {
		connect(button, &QPushButton::clicked, &dialog, [&, button] {
			const QColor picked = QColorDialog::getColor(QColor(button->text()), &dialog);
			if (picked.isValid()) {
				setColor(*button, picked);
				refresh();
			}
		});
	}
	QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	layout->addWidget(&buttons);
	connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	preview->Pause();
	previewTimer->stop();
	play->setText(Text("Play"));
	load(cue.style);
	dialog.resize(std::max(440, form.sizeHint().width() + 55), std::min(650, height()));
	dialog.move(mapToGlobal(QPoint(std::max(0, width() - dialog.width()), 0)));
	if (dialog.exec() == QDialog::Accepted) {
		auto cues = project.Clips()[target].subtitles;
		const auto value = style();
		if (all.isChecked()) {
			for (auto &item : cues)
				item.style = value;
		} else {
			cues[row].style = value;
		}
		ChangeSubtitles(target, std::move(cues), row);
	} else {
		UpdateSubtitlePreview(playheadSource);
	}
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
	if (left.words.size() == words.size() &&
	    std::all_of(left.words.begin(), left.words.end(), [](const auto &word) { return word.Known(); }))
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
	const int previousStart = cue.inMs;
	cue.outMs = position + std::min(cue.outMs - cue.inMs, clip.outMs - position);
	cue.inMs = position;
	for (auto &word : cue.words) {
		if (!word.Known())
			continue;
		const int duration = cue.outMs - cue.inMs;
		word.inMs = position + std::clamp(word.inMs - previousStart, 0, duration);
		word.outMs = position + std::clamp(word.outMs - previousStart, 0, duration);
		if (!word.Known())
			word = {0, 0};
	}
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
			if (!word.Known())
				continue;
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
	const QStringList filters{u"SubRip (*.srt)"_s, u"WebVTT (*.vtt)"_s, u"Advanced SubStation Alpha (*.ass)"_s};
	const QStringList formats{u".srt"_s, u".vtt"_s, u".ass"_s};
	QString filter = filters.first();
	QString path = QFileDialog::getSaveFileName(this, Text("ExportSubtitleFile"), {}, filters.join(u";;"_s), &filter);
	if (path.isEmpty())
		return;
	if (std::none_of(formats.begin(), formats.end(), [&](const auto &suffix) {
		    return path.endsWith(suffix, Qt::CaseInsensitive);
	    }))
		path += formats[std::max<qsizetype>(0, filters.indexOf(filter))];
	if (!project.ExportSubtitles(path))
		QMessageBox::warning(this, Text("Subtitles"), Text("SubtitleExportFailed"));
}

void EditorWindow::ExportCaptionVideo()
{
	const QStringList choices{Text("CaptionBurnIn"), Text("CaptionSelectable")};
	bool accepted;
	const auto choice = QInputDialog::getItem(this, Text("ExportCaptionVideo"), Text("CaptionChooseMode"),
						  choices, 0, false, &accepted);
	if (!accepted)
		return;
	const bool burnIn = choice == choices.first();
	QString executable = speechOptions.ffmpeg;
	if (executable.isEmpty())
		executable = QStandardPaths::findExecutable(u"ffmpeg"_s);
	executable = QFileDialog::getOpenFileName(this, Text(burnIn ? "CaptionFfmpegAss" : "CaptionFfmpeg"), executable);
	if (!QFileInfo(executable).isExecutable())
		return;
	QString path = QFileDialog::getSaveFileName(this, Text("ExportCaptionVideo"), {}, u"Matroska (*.mkv)"_s);
	if (path.isEmpty())
		return;
	if (!path.endsWith(u".mkv"_s, Qt::CaseInsensitive))
		path += u".mkv"_s;
	if (QFileInfo::exists(path)) {
		QMessageBox::warning(this, Text("ExportCaptionVideo"), Text("CaptionVideoExists"));
		return;
	}
	const auto fail = [this](const QString &details) {
		QMessageBox message(QMessageBox::Warning, Text("ExportCaptionVideo"), {}, QMessageBox::Ok, this);
		message.setTextFormat(Qt::PlainText);
		message.setText(Text("CaptionVideoFailed").arg(details));
		message.exec();
	};
	auto staging = std::make_shared<QTemporaryDir>(QFileInfo(path).absolutePath() + u"/.obs-caption-XXXXXX"_s);
	if (!staging->isValid()) {
		fail(staging->errorString());
		return;
	}
	QString failedMedia;
	const auto args = project.VideoExportArguments(staging->path(), burnIn, failedMedia);
	if (args.isEmpty()) {
		fail(failedMedia.isEmpty() ? Text("SubtitleExportFailed") : Text("CaptionMissingMedia").arg(failedMedia));
		return;
	}
	auto *progress = new QProgressDialog(Text("CaptionRendering").arg(0), Text("SubtitleCancel"), 0, 100, this);
	progress->setWindowTitle(Text("ExportCaptionVideo"));
	progress->setWindowModality(Qt::WindowModal);
	progress->setAutoClose(false);
	progress->setAutoReset(false);
	progress->setMinimumDuration(0);
	progress->setValue(0);
	auto *process = new QProcess(progress);
	process->setWorkingDirectory(staging->path());
	connect(progress, &QProgressDialog::canceled, process, &QProcess::kill);
	connect(process, &QProcess::readyReadStandardOutput, progress,
		[this, progress, process, duration = project.Duration()] {
			while (process->canReadLine()) {
				const auto line = process->readLine();
				if (progress->wasCanceled() || !line.startsWith("out_time_us="))
					continue;
				const auto time = std::max<qint64>(0, line.mid(12).trimmed().toLongLong());
				const int percent = static_cast<int>(std::min<qint64>(99, time / (10LL * duration)));
				progress->setLabelText(Text("CaptionRendering").arg(percent));
				progress->setValue(percent);
			}
		});
	connect(process, &QProcess::errorOccurred, progress, [progress, process, fail](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) {
			progress->close();
			progress->deleteLater();
			fail(process->errorString());
		}
	});
	connect(process, &QProcess::finished, progress,
		[this, process, progress, staging, path, fail, burnIn](int exitCode, QProcess::ExitStatus status) {
			const bool canceled = progress->wasCanceled();
			progress->hide();
			progress->deleteLater();
			if (canceled)
				return;
			if (exitCode || status != QProcess::NormalExit) {
				auto details = QString::fromUtf8(process->readAllStandardError().right(1800)).trimmed();
				if (burnIn && (details.contains(u"filter not found"_s, Qt::CaseInsensitive) ||
					       details.contains(u"no such filter"_s, Qt::CaseInsensitive)))
					details += u'\n' + Text("CaptionFfmpegAss");
				fail(details);
				return;
			}
			QFile output(staging->filePath(u"output.mkv"_s));
			if (!output.rename(path)) {
				fail(output.errorString());
				return;
			}
			statusBar()->showMessage(Text("Exported").arg(QDir::toNativeSeparators(path)), 10000);
		});
	progress->show();
	process->start(executable, args);
}
