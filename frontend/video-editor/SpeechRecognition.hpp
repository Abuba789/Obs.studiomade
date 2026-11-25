#pragma once

#include "EditorProject.hpp"

#include <QProcess>
#include <QTemporaryDir>

#include <functional>

class SpeechRecognition : public QObject {
public:
	struct Options {
		QString ffmpeg;
		QString whisper;
		QString model;
		QString language;
		bool cpu = false;
	};

	SpeechRecognition(Options options, QString media, int inMs, int outMs, QObject *parent);
	~SpeechRecognition() override;
	void Start();
	void Cancel();
	std::function<void(int, bool)> progress;
	std::function<void(QVector<VideoEditor::Subtitle>, QString, bool)> completed;

private:
	QProcess process;
	QTemporaryDir temporary;
	Options options;
	QString media;
	int start;
	int end;
	int cursor;
	int chunkEnd = 0;
	bool recognizing = false;
	bool canceled = false;
	QByteArray log;
	QVector<VideoEditor::Subtitle> subtitles;

	void Extract();
	void Recognize();
	void ReadProgress();
	void Finish(int code, QProcess::ExitStatus status);
	bool ReadResult();
};
