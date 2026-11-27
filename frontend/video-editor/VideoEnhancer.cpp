#include "EditorWindow.hpp"
#include "EditorPreview.hpp"

#include <OBSApp.hpp>
#include <utility/platform.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <memory>

using namespace Qt::StringLiterals;

namespace {
class VideoComparison : public QWidget {
public:
	QImage original;
	QImage enhanced;
	int mode = 2;
	int divider = 50;

	explicit VideoComparison(QWidget *parent) : QWidget(parent) { setMinimumSize(500, 260); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.fillRect(rect(), palette().base());
		painter.setRenderHint(QPainter::SmoothPixmapTransform);
		const auto draw = [&painter](const QImage &image, QRect area) {
			QRect target(QPoint(), image.size().scaled(area.size(), Qt::KeepAspectRatio));
			target.moveCenter(area.center());
			painter.drawImage(target, image);
		};
		if (original.isNull() || enhanced.isNull())
			return;
		if (mode == 0) {
			draw(original, rect());
		} else if (mode == 1) {
			draw(enhanced, rect());
		} else if (mode == 2) {
			draw(original, QRect(0, 0, width() / 2, height()));
			draw(enhanced, QRect(width() / 2, 0, width() / 2, height()));
		} else {
			const int x = width() * divider / 100;
			draw(original, rect());
			painter.setClipRect(QRect(x, 0, width() - x, height()));
			draw(enhanced, rect());
			painter.setClipping(false);
			painter.setPen(palette().highlight().color());
			painter.drawLine(x, 0, x, height());
		}
	}
};

struct Model {
	const char *name;
	const char *url;
	std::array<bool, 7> filters;
};

constexpr Model models[] = {
	{"RealESRNet x4plus", "https://github.com/xinntao/Real-ESRGAN/blob/master/docs/model_zoo.md",
	 {true, true, false, false, false, false, false}},
	{"RealESRGAN x4plus", "https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.1.0",
	 {false, true, true, true, false, false, false}},
	{"RealESRGAN AnimeVideo v3", "https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0",
	 {false, false, false, false, false, false, true}},
	{"RealESRGAN x2plus", "https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.1",
	 {true, true, true, true, true, false, false}},
};

QStringList MuxArguments(const QString &video, const QString &source, const QString &output, int start, int end,
			 bool keepAudio)
{
	const auto seconds = [](int ms) { return QString::number(ms / 1000.0, 'f', 3); };
	QStringList args{u"-nostdin"_s, u"-hide_banner"_s, u"-loglevel"_s, u"error"_s, u"-n"_s,
			 u"-i"_s, video};
	if (keepAudio)
		args += {u"-protocol_whitelist"_s, u"file"_s, u"-ss"_s, seconds(start), u"-i"_s, source,
			 u"-map"_s, u"0:v:0"_s, u"-map"_s, u"1:a?"_s, u"-c:a"_s, u"aac"_s, u"-b:a"_s, u"192k"_s};
	else
		args += {u"-map"_s, u"0:v:0"_s};
	args += {u"-c:v"_s, u"copy"_s, u"-t"_s, seconds(end - start), output};
	return args;
}
}

void EditorWindow::EnhanceVideo()
{
	if (selectedClip < 0 || project.Assets()[project.Clips()[selectedClip].asset].track != VideoEditor::Track::Video)
		return;
	std::string workerPath;
	if (!GetDataFilePath("video-editor/ai_upscaler.py", workerPath)) {
		QMessageBox::warning(this, Text("Enhancer"), Text("EnhancerMissingWorker"));
		return;
	}
	const int initialClip = selectedClip;
	const int initialOffset = seek->value();
	preview->Pause();
	previewTimer->stop();
	play->setText(Text("Play"));

	QDialog dialog(this);
	dialog.setWindowTitle(Text("Enhancer"));
	QVBoxLayout layout(&dialog);
	QTabWidget tabs(&dialog);
	layout.addWidget(&tabs);
	auto *configuration = new QWidget(&tabs);
	auto *form = new QFormLayout(configuration);
	tabs.addTab(configuration, Text("EnhancerSettings"));
	auto *modelTab = new QWidget(&tabs);
	auto *modelForm = new QFormLayout(modelTab);
	tabs.addTab(modelTab, Text("EnhancerModels"));

	BPtr<char> settingsPath = GetAppConfigPathPtr("obs-studio/editor-video-enhancer.ini");
	QSettings settings(QString::fromUtf8(settingsPath.Get()), QSettings::IniFormat);
	const auto fileField = [&](const QString &label, const QString &initial, const QString &filter) {
		auto *container = new QWidget(modelTab);
		auto *row = new QHBoxLayout(container);
		row->setContentsMargins(0, 0, 0, 0);
		auto *field = new QLineEdit(initial, container);
		auto *browse = new QPushButton(Text("SubtitleBrowse"), container);
		row->addWidget(field);
		row->addWidget(browse);
		connect(browse, &QPushButton::clicked, &dialog, [&, field, label, filter] {
			const auto chosen = QFileDialog::getOpenFileName(&dialog, label, field->text(), filter);
			if (!chosen.isEmpty())
				field->setText(chosen);
		});
		modelForm->addRow(label, container);
		return field;
	};
	auto *python = fileField(u"Python"_s, settings.value(u"python"_s, QStandardPaths::findExecutable(u"python"_s)).toString(), {});
	auto *ffmpeg = fileField(u"FFmpeg"_s, settings.value(u"ffmpeg"_s, QStandardPaths::findExecutable(u"ffmpeg"_s)).toString(), {});
	QComboBox profile(configuration);
	profile.addItems({Text("EnhancerNatural"), Text("EnhancerDefinition"), Text("EnhancerAnime"), Text("EnhancerVintage")});
	form->addRow(Text("EnhancerProfile"), &profile);
	auto *model = fileField(Text("EnhancerModel"), {}, u"PyTorch (*.pth *.pt);;*"_s);
	QLabel license(modelTab);
	license.setOpenExternalLinks(true);
	license.setWordWrap(true);
	modelForm->addRow(&license);
	auto *setup = new QLabel(Text("EnhancerSetup"), modelTab);
	setup->setWordWrap(true);
	setup->setOpenExternalLinks(true);
	modelForm->addRow(setup);
	const auto updateModelInfo = [&] {
		const auto &recommended = models[profile.currentIndex()];
		const auto size = QFileInfo(model->text()).size();
		license.setText(Text("EnhancerModelInfo").arg(QString::fromLatin1(recommended.url),
							     QString::fromLatin1(recommended.name),
							     QString::number(size / 1048576.0, 'f', 1)));
	};
	connect(model, &QLineEdit::textChanged, &dialog, updateModelInfo);

	QComboBox resolution(configuration), framing(configuration), device(configuration), codec(configuration), format(configuration);
	resolution.addItem(Text("EnhancerOriginalResolution"), 0);
	for (int height : {1080, 1440, 2160})
		resolution.addItem(QString::number(height) + u"p"_s, height);
	resolution.setCurrentIndex(1);
	framing.addItem(Text("EnhancerFit"), u"fit"_s);
	framing.addItem(Text("EnhancerPad"), u"pad"_s);
	framing.addItem(Text("EnhancerCrop"), u"crop"_s);
	device.addItem(Text("SubtitleAuto"), u"auto"_s);
	device.addItem(u"CUDA"_s, u"cuda"_s);
	device.addItem(u"CPU"_s, u"cpu"_s);
	codec.addItem(u"H.264"_s, u"libx264"_s);
	codec.addItem(u"HEVC"_s, u"libx265"_s);
	format.addItems({u"MP4"_s, u"MKV"_s, u"MOV"_s});
	form->addRow(Text("EnhancerResolution"), &resolution);
	form->addRow(Text("EnhancerFraming"), &framing);
	form->addRow(Text("EnhancerDevice"), &device);
	form->addRow(Text("EnhancerCodec"), &codec);
	form->addRow(Text("EnhancerFormat"), &format);

	QSpinBox fps(configuration), quality(configuration), bitrate(configuration), batch(configuration);
	fps.setRange(0, 240);
	fps.setSpecialValueText(Text("EnhancerOriginalResolution"));
	quality.setRange(12, 35);
	quality.setValue(19);
	bitrate.setRange(0, 150000);
	bitrate.setSpecialValueText(Text("EnhancerUseQuality"));
	batch.setRange(1, 4);
	QComboBox tile(configuration);
	for (int size : {64, 128, 256})
		tile.addItem(QString::number(size) + u" px"_s, size);
	tile.setCurrentIndex(1);
	form->addRow(Text("EnhancerFps"), &fps);
	form->addRow(Text("EnhancerQuality"), &quality);
	form->addRow(Text("EnhancerBitrate"), &bitrate);
	form->addRow(Text("EnhancerTile"), &tile);
	form->addRow(Text("EnhancerBatch"), &batch);

	auto *filtersTab = new QWidget(&tabs);
	auto *filtersLayout = new QVBoxLayout(filtersTab);
	tabs.addTab(filtersTab, Text("EnhancerFilters"));
	const std::array<const char *, 7> filterKeys{"noise", "sharpness", "details", "deblock", "contrast", "color", "antialias"};
	const std::array<const char *, 7> filterLabels{"EnhancerNoise", "EnhancerSharpness", "EnhancerDetails",
						     "EnhancerDeblock", "EnhancerContrast", "EnhancerColor", "EnhancerAntialias"};
	std::array<QCheckBox *, 7> filters{};
	for (size_t i = 0; i < filters.size(); ++i) {
		filters[i] = new QCheckBox(Text(filterLabels[i]), filtersTab);
		filtersLayout->addWidget(filters[i]);
	}
	filtersLayout->addStretch();
	const auto modelKey = [](int index) { return QStringLiteral("models/%1").arg(index); };
	const auto setProfile = [&](int index) {
		model->setText(settings.value(modelKey(index)).toString());
		updateModelInfo();
		for (size_t i = 0; i < filters.size(); ++i)
			filters[i]->setChecked(models[index].filters[i]);
	};
	connect(&profile, &QComboBox::currentIndexChanged, &dialog, setProfile);
	connect(model, &QLineEdit::editingFinished, &dialog, [&] {
		settings.setValue(modelKey(profile.currentIndex()), model->text());
	});
	setProfile(0);

	auto *clipsTab = new QWidget(&tabs);
	auto *clipsLayout = new QVBoxLayout(clipsTab);
	QListWidget clips(clipsTab);
	QCheckBox whole(Text("EnhancerWhole"), clipsTab), replace(Text("EnhancerReplace"), clipsTab),
		audio(Text("EnhancerAudio"), clipsTab);
	audio.setChecked(true);
	clipsLayout->addWidget(&clips);
	clipsLayout->addWidget(&whole);
	clipsLayout->addWidget(&audio);
	clipsLayout->addWidget(&replace);
	tabs.addTab(clipsTab, Text("Timeline"));
	for (qsizetype i = 0; i < project.Clips().size(); ++i) {
		const auto &clip = project.Clips()[i];
		const auto &asset = project.Assets()[clip.asset];
		if (asset.track != VideoEditor::Track::Video)
			continue;
		auto *item = new QListWidgetItem(Text("EnhancerClip").arg(QFileInfo(asset.path).fileName()).arg(clip.startMs / 1000.0), &clips);
		item->setData(Qt::UserRole, static_cast<int>(i));
		item->setCheckState(i == initialClip ? Qt::Checked : Qt::Unchecked);
	}

	auto *comparisonTab = new QWidget(&tabs);
	auto *comparisonLayout = new QVBoxLayout(comparisonTab);
	VideoComparison comparison(comparisonTab);
	QComboBox mode(comparisonTab);
	mode.addItems({Text("EnhancerOriginal"), Text("EnhancerEnhanced"), Text("EnhancerSideBySide"), Text("EnhancerDivider")});
	mode.setCurrentIndex(2);
	QSlider divider(Qt::Horizontal, comparisonTab);
	divider.setRange(0, 100);
	divider.setValue(50);
	divider.setEnabled(false);
	QSpinBox frameTime(comparisonTab);
	frameTime.setRange(0, project.Clips()[initialClip].Duration() - 1);
	frameTime.setValue(initialOffset);
	frameTime.setSuffix(u" ms"_s);
	frameTime.setAccessibleName(Text("EnhancerFrame"));
	QPushButton previewButton(Text("EnhancerPreview"), comparisonTab);
	comparisonLayout->addWidget(&comparison);
	comparisonLayout->addWidget(&mode);
	comparisonLayout->addWidget(&divider);
	comparisonLayout->addWidget(new QLabel(Text("EnhancerFrame"), comparisonTab));
	comparisonLayout->addWidget(&frameTime);
	comparisonLayout->addWidget(&previewButton);
	tabs.addTab(comparisonTab, Text("Preview"));
	connect(&mode, &QComboBox::currentIndexChanged, &dialog, [&](int value) {
		comparison.mode = value;
		divider.setEnabled(value == 3);
		comparison.update();
	});
	connect(&divider, &QSlider::valueChanged, &dialog, [&](int value) {
		comparison.divider = value;
		comparison.update();
	});

	QLabel status(&dialog);
	status.setWordWrap(true);
	status.setTextFormat(Qt::PlainText);
	QProgressBar progress(&dialog);
	QDialogButtonBox actions(&dialog);
	auto *startButton = actions.addButton(Text("EnhancerStart"), QDialogButtonBox::ActionRole);
	auto *cancel = actions.addButton(Text("SubtitleCancel"), QDialogButtonBox::ActionRole);
	auto *close = actions.addButton(QDialogButtonBox::Close);
	layout.addWidget(&status);
	layout.addWidget(&progress);
	layout.addWidget(&actions);
	cancel->setEnabled(false);
	connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);

	struct Result {
		int clip;
		int sourceStart;
		bool replace;
		QString path;
	};
	QVector<Result> completed;
	QVector<int> queue;
	std::unique_ptr<QTemporaryDir> staging;
	QProcess worker;
	QString folder, destination, source, hardware, errors;
	int cursor = 0, sourceStart = 0, sourceEnd = 0;
	bool isPreview = false, muxing = false, canceled = false, busy = false;

	const auto setBusy = [&](bool value) {
		busy = value;
		tabs.setEnabled(!value);
		startButton->setEnabled(!value);
		cancel->setEnabled(value);
		close->setEnabled(!value);
	};
	const auto stop = [&](const QString &message) {
		setBusy(false);
		staging.reset();
		status.setText(message);
	};
	connect(cancel, &QPushButton::clicked, &dialog, [&] {
		canceled = true;
		worker.kill();
	});
	connect(&worker, &QProcess::readyReadStandardError, &dialog, [&] {
		errors = (errors + QString::fromUtf8(worker.readAllStandardError())).right(2500);
	});
	connect(&worker, &QProcess::readyReadStandardOutput, &dialog, [&] {
		while (worker.canReadLine()) {
			const auto event = QJsonDocument::fromJson(worker.readLine()).object();
			if (event.contains("device"_L1)) {
				hardware = event["device"_L1].toString();
				status.setText(hardware);
			}
			if (!event.contains("frames"_L1))
				continue;
			const int percent = event["percent"_L1].toInt();
			progress.setValue((100 * cursor + percent) / static_cast<int>(queue.size()));
			const auto total = event["total"_L1].isDouble() ? QString::number(event["total"_L1].toInt()) : u"?"_s;
			auto message = Text("EnhancerProgress").arg(hardware).arg(event["frames"_L1].toInt()).arg(total)
					       .arg(event["speed"_L1].toDouble(), 0, 'f', 2);
			if (event["eta"_L1].isDouble())
				message += u" · "_s + Text("EnhancerEta").arg(event["eta"_L1].toInt());
			if (event["memoryMiB"_L1].isDouble())
				message += u" · "_s + Text("EnhancerMemory").arg(event["memoryMiB"_L1].toInt());
			status.setText(message);
		}
	});

	const auto launch = [&] {
		const auto &clip = project.Clips()[queue[cursor]];
		source = project.Assets()[clip.asset].path;
		sourceStart = !isPreview && whole.isChecked() ? 0 : clip.inMs;
		sourceEnd = !isPreview && whole.isChecked() ? project.Assets()[clip.asset].durationMs : clip.outMs;
		staging = isPreview ? std::make_unique<QTemporaryDir>()
				    : std::make_unique<QTemporaryDir>(QDir(folder).filePath(u".obs-enhancer-XXXXXX"_s));
		if (!staging->isValid()) {
			stop(Text("EnhancerError").arg(staging->errorString()));
			return;
		}
		if (!isPreview) {
			const auto extension = u'.' + format.currentText().toLower();
			const auto base = QFileInfo(source).completeBaseName() + u"-enhanced"_s;
			destination = QDir(folder).filePath(base + extension);
			for (int suffix = 2; QFileInfo::exists(destination); ++suffix)
				destination = QDir(folder).filePath(base + QStringLiteral("-%1").arg(suffix) + extension);
		}
		QJsonObject options{{"model"_L1, QFileInfo(model->text()).absoluteFilePath()}, {"source"_L1, source},
				    {"output"_L1, staging->filePath(u"video.mkv"_s)}, {"startMs"_L1, sourceStart},
				    {"endMs"_L1, sourceEnd}, {"preview"_L1, isPreview},
				    {"previewMs"_L1, clip.inMs + frameTime.value()}, {"whole"_L1, whole.isChecked()},
				    {"height"_L1, resolution.currentData().toInt()}, {"framing"_L1, framing.currentData().toString()},
				    {"device"_L1, device.currentData().toString()}, {"tile"_L1, tile.currentData().toInt()},
				    {"batch"_L1, batch.value()}, {"codec"_L1, codec.currentData().toString()}, {"fps"_L1, fps.value()},
				    {"bitrate"_L1, bitrate.value()}, {"quality"_L1, quality.value()}};
		for (size_t i = 0; i < filters.size(); ++i)
			options.insert(QString::fromLatin1(filterKeys[i]), filters[i]->isChecked());
		QSaveFile config(staging->filePath(u"options.json"_s));
		const auto json = QJsonDocument(options).toJson(QJsonDocument::Compact);
		if (!config.open(QIODevice::WriteOnly) || config.write(json) != json.size() || !config.commit()) {
			config.cancelWriting();
			stop(Text("EnhancerError").arg(config.errorString()));
			return;
		}
		muxing = false;
		errors.clear();
		status.setText(Text("EnhancerLoading"));
		worker.start(QFileInfo(python->text()).absoluteFilePath(),
			     {u"-I"_s, QString::fromStdString(workerPath), config.fileName()});
	};
	connect(&worker, &QProcess::errorOccurred, &dialog, [&](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart)
			stop(Text("EnhancerError").arg(worker.errorString()));
	});
	connect(&worker, &QProcess::finished, &dialog, [&](int code, QProcess::ExitStatus result) {
		if (canceled || code != 0 || result != QProcess::NormalExit) {
			stop(canceled ? Text("EnhancerCanceled") : Text("EnhancerError").arg(errors));
			return;
		}
		if (isPreview) {
			const QImage original(staging->filePath(u"original.png"_s));
			const QImage enhanced(staging->filePath(u"enhanced.png"_s));
			if (original.isNull() || enhanced.isNull()) {
				stop(Text("EnhancerError").arg(Text("EnhancerMissingPreview")));
				return;
			}
			comparison.original = original;
			comparison.enhanced = enhanced;
			comparison.update();
			progress.setValue(100);
			stop(Text("EnhancerPreviewDone").arg(hardware).arg(enhanced.width()).arg(enhanced.height()));
			return;
		}
		const auto temporaryOutput = staging->filePath(u"result."_s + format.currentText().toLower());
		if (!muxing) {
			muxing = true;
			errors.clear();
			status.setText(Text("EnhancerMuxing"));
			worker.start(QFileInfo(ffmpeg->text()).absoluteFilePath(),
				     MuxArguments(staging->filePath(u"video.mkv"_s), source, temporaryOutput,
						  sourceStart, sourceEnd, audio.isChecked()));
			return;
		}
		if (!QFile::rename(temporaryOutput, destination)) {
			stop(Text("EnhancerError").arg(destination));
			return;
		}
		completed.append({queue[cursor], sourceStart, replace.isChecked(), destination});
		if (++cursor == queue.size()) {
			progress.setValue(100);
			stop(Text("EnhancerFinished"));
		} else {
			QTimer::singleShot(0, &dialog, [&] {
				if (canceled)
					stop(Text("EnhancerCanceled"));
				else
					launch();
			});
		}
	});

	const auto begin = [&](bool previewOnly) {
		if (busy)
			return;
		if (!QFileInfo(python->text()).isExecutable() || (!previewOnly && !QFileInfo(ffmpeg->text()).isExecutable()) ||
		    !QFileInfo(model->text()).isFile()) {
			status.setText(Text("EnhancerTools"));
			tabs.setCurrentWidget(modelTab);
			return;
		}
		isPreview = previewOnly;
		queue.clear();
		if (previewOnly) {
			queue.append(initialClip);
		} else {
			for (int row = 0; row < clips.count(); ++row) {
				const auto *item = clips.item(row);
				if (item->checkState() == Qt::Checked)
					queue.append(item->data(Qt::UserRole).toInt());
			}
			if (queue.isEmpty()) {
				status.setText(Text("EnhancerChooseClips"));
				return;
			}
			folder = QFileDialog::getExistingDirectory(&dialog, Text("EnhancerDestination"), folder,
								   QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
			if (folder.isEmpty())
				return;
		}
		settings.setValue(u"python"_s, python->text());
		settings.setValue(u"ffmpeg"_s, ffmpeg->text());
		settings.setValue(modelKey(profile.currentIndex()), model->text());
		settings.sync();
		canceled = false;
		cursor = 0;
		progress.setValue(0);
		setBusy(true);
		launch();
	};
	connect(&previewButton, &QPushButton::clicked, &dialog, [&] { begin(true); });
	connect(startButton, &QPushButton::clicked, &dialog, [&] { begin(false); });
	dialog.resize(710, 660);
	dialog.exec();
	canceled = true;
	worker.disconnect(&dialog);
	if (worker.state() != QProcess::NotRunning) {
		worker.kill();
		worker.waitForFinished(2000);
	}

	for (const auto &result : completed) {
		if (!project.AddEnhancedVersion(result.path, result.clip, result.sourceStart, result.replace)) {
			QMessageBox::warning(this, Text("Enhancer"), Text("EnhancerImportFailed").arg(result.path));
			continue;
		}
		if (result.replace) {
			AdjustSubtitleJob(result.clip, 0);
			subtitleUndo->clear();
		}
		modified = true;
	}
	if (!completed.isEmpty()) {
		UpdateLibrary();
		library->setCurrentRow(static_cast<int>(project.Assets().size()) - 1);
		SelectClip(initialClip);
		UpdateTimeline();
	}
}
