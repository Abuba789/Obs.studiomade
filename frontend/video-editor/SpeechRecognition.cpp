#include "SpeechRecognition.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;
using VideoEditor::Subtitle;

SpeechRecognition::SpeechRecognition(Options options, QString media, int inMs, int outMs, QObject *parent)
	: QObject(parent),
	  options(std::move(options)),
	  media(std::move(media)),
	  start(inMs),
	  end(outMs),
	  cursor(inMs)
{
	process.setStandardOutputFile(QProcess::nullDevice());
	connect(&process, &QProcess::readyReadStandardError, this, &SpeechRecognition::ReadProgress);
	connect(&process, &QProcess::finished, this, &SpeechRecognition::Finish);
	connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart)
			completed({}, process.errorString(), canceled);
	});
}

SpeechRecognition::~SpeechRecognition()
{
	disconnect(&process, nullptr, this, nullptr);
	if (process.state() != QProcess::NotRunning) {
		process.kill();
		process.waitForFinished();
	}
}

void SpeechRecognition::Start()
{
	if (!temporary.isValid()) {
		completed({}, temporary.errorString(), false);
		return;
	}
	Extract();
}

void SpeechRecognition::Cancel()
{
	canceled = true;
	process.kill();
}

void SpeechRecognition::Extract()
{
	recognizing = false;
	log.clear();
	chunkEnd = cursor + std::min(600000, end - cursor);
	progress(static_cast<int>(100LL * (cursor - start) / (end - start)), false);
	process.start(options.ffmpeg,
		      {u"-nostdin"_s, u"-hide_banner"_s, u"-loglevel"_s, u"error"_s, u"-progress"_s, u"pipe:2"_s, u"-y"_s,
		       u"-protocol_whitelist"_s, u"file"_s, u"-ss"_s, QString::number(cursor / 1000.0, 'f', 3),
		       u"-i"_s, media, u"-t"_s, QString::number((chunkEnd - cursor) / 1000.0, 'f', 3),
		       u"-map"_s, u"0:a:0"_s, u"-af"_s, u"aresample=async=1:first_pts=0"_s,
		       u"-ac"_s, u"1"_s, u"-ar"_s, u"16000"_s, u"-c:a"_s, u"pcm_s16le"_s,
		       temporary.filePath(u"audio.wav"_s)});
}

void SpeechRecognition::Recognize()
{
	recognizing = true;
	log.clear();
	QFile::remove(temporary.filePath(u"captions.json"_s));
	progress(static_cast<int>((100LL * (cursor - start) + 10LL * (chunkEnd - cursor)) / (end - start)), true);
	QStringList args{u"-m"_s, options.model, u"-f"_s, temporary.filePath(u"audio.wav"_s),
			 u"-l"_s, options.language, u"-ojf"_s, u"-of"_s, temporary.filePath(u"captions"_s),
			 u"-pp"_s, u"-ml"_s, u"84"_s, u"-sow"_s, u"-t"_s,
			 QString::number(std::clamp(QThread::idealThreadCount() / 2, 1, 4))};
	if (options.cpu)
		args.append(u"-ng"_s);
	process.start(options.whisper, args);
}

void SpeechRecognition::ReadProgress()
{
	log = (log + process.readAllStandardError()).right(8192);
	static const QRegularExpression pattern(u"progress\\s*=\\s*(\\d+)%"_s);
	static const QRegularExpression extraction(u"out_time_us=(\\d+)"_s);
	auto matches = (recognizing ? pattern : extraction).globalMatch(QString::fromUtf8(log));
	qint64 value = 0;
	while (matches.hasNext())
		value = matches.next().captured(1).toLongLong();
	if (value > 0) {
		const qint64 length = chunkEnd - cursor;
		const qint64 chunkProgress = recognizing ? (10 + std::clamp<qint64>(value, 0, 100) * 9 / 10) * length
							  : std::min(length, value / 1000) * 10;
		const qint64 done = 100LL * (cursor - start) + chunkProgress;
		progress(std::min(99, static_cast<int>(done / (end - start))), recognizing);
	}
}

void SpeechRecognition::Finish(int code, QProcess::ExitStatus status)
{
	ReadProgress();
	if (canceled) {
		completed({}, {}, true);
	} else if (code || status != QProcess::NormalExit) {
		const auto details = QStringLiteral("0x%1\n%2").arg(static_cast<unsigned int>(code), 8, 16, u'0')
					     .arg(QString::fromUtf8(log.right(1500)).trimmed());
		completed({}, details, false);
	} else if (!recognizing) {
		Recognize();
	} else if (!ReadResult()) {
		completed({}, QString::fromUtf8(log.right(1500)).trimmed(), false);
	} else if (chunkEnd < end) {
		cursor = chunkEnd;
		Extract();
	} else {
		progress(100, true);
		completed(std::move(subtitles), {}, false);
	}
}

bool SpeechRecognition::ReadResult()
{
	QFile file(temporary.filePath(u"captions.json"_s));
	if (!file.open(QIODevice::ReadOnly))
		return false;
	const auto document = QJsonDocument::fromJson(file.readAll()).object();
	const auto segments = document["transcription"_L1];
	if (!segments.isArray())
		return false;
	const auto language = document["result"_L1].toObject()["language"_L1].toString(options.language);
	const auto position = [this](const QJsonValue &value) {
		return cursor + std::clamp(value.toInt(), 0, chunkEnd - cursor);
	};
	for (const auto &segment : segments.toArray()) {
		const auto data = segment.toObject();
		const auto offsets = data["offsets"_L1].toObject();
		Subtitle cue{position(offsets["from"_L1]), position(offsets["to"_L1]),
			     data["text"_L1].toString().simplified(), language, {}};
		if (cue.text.isEmpty() || cue.inMs >= cue.outMs)
			continue;
		QString tokenText;
		struct Token {
			qsizetype from;
			qsizetype to;
			int start;
			int end;
			bool lexical;
		};
		QVector<Token> tokens;
		for (const auto &entry : data["tokens"_L1].toArray()) {
			const auto token = entry.toObject();
			const auto text = token["text"_L1].toString();
			if (text.startsWith(u"[_"_s))
				continue;
			const auto times = token["offsets"_L1].toObject();
			const auto from = tokenText.size();
			tokenText += text;
			tokens.append({from, tokenText.size(), position(times["from"_L1]), position(times["to"_L1]),
				       std::any_of(text.begin(), text.end(), [](QChar c) { return c.isLetterOrNumber(); })});
		}
		if (tokenText.simplified() == cue.text) {
			static const QRegularExpression word(u"\\S+"_s);
			auto matches = word.globalMatch(tokenText);
			while (matches.hasNext()) {
				const auto match = matches.next();
				VideoEditor::SubtitleTiming timing{cue.outMs, cue.inMs};
				for (const auto &token : tokens) {
					if (token.from < match.capturedEnd() && token.to > match.capturedStart() &&
					    token.end > token.start && token.lexical) {
						timing.inMs = std::min(timing.inMs, std::max(token.start, cue.inMs));
						timing.outMs = std::max(timing.outMs, std::min(token.end, cue.outMs));
					}
				}
				cue.words.append(timing.Known() ? timing : VideoEditor::SubtitleTiming{0, 0});
			}
			if (std::none_of(cue.words.begin(), cue.words.end(), [](const auto &timing) { return timing.Known(); }))
				cue.words.clear();
		}
		subtitles.append(std::move(cue));
	}
	return true;
}
