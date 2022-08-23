#include "zvideoencoder.h"
#include "zlog.h"
#include "zsysteminfo.h"

namespace nim {

ZVideoEncoder::ZVideoEncoder(QObject* parent)
  : QObject(parent)
{
  m_ffmpegProcess = new QProcess(this);
  connect(m_ffmpegProcess, &QProcess::errorOccurred, this, &ZVideoEncoder::ffmpegError);
  connect(m_ffmpegProcess,
          qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this,
          &ZVideoEncoder::ffmpegFinished);
  connect(m_ffmpegProcess, &QProcess::readyReadStandardOutput, this, &ZVideoEncoder::logStandardOutput);
  connect(m_ffmpegProcess, &QProcess::readyReadStandardError, this, &ZVideoEncoder::logStandardError);
}

void ZVideoEncoder::encode(const QDir& dir,
                           const QString& namePrefix,
                           int fieldWidth,
                           int framesPerSecond,
                           const QString& outputFilename)
{
  if (m_ffmpegProcess->state() != QProcess::NotRunning) {
    Q_EMIT error("Encoder is already running.");
    return;
  }

#ifdef _WIN32
  QString program = ZSystemInfo::resourceDir().absoluteFilePath("ffmpeg.exe");
#else
  QString program = ZSystemInfo::resourceDir().absoluteFilePath("ffmpeg");
#endif
  QStringList arguments;
  arguments << "-r" << QString::number(framesPerSecond, 'f', 2) << "-i"
            << (QString("%1/%2% 0%3d.png").arg(dir.absolutePath()).arg(namePrefix, fieldWidth).replace("% 0", "%0"))
            << "-c:v"
            << "libx264"
            << "-crf"
            << "18"
            << "-pix_fmt"
            << "yuv420p"
            << "-r" << QString::number(framesPerSecond, 'f', 2) << outputFilename;
  LOG(INFO) << program << " " << arguments.join(" ");
  m_ffmpegProcess->start(program, arguments);
}

void ZVideoEncoder::cancel()
{
  blockSignals(true);
  m_ffmpegProcess->kill();
  m_ffmpegProcess->waitForFinished();
  blockSignals(false);
  Q_EMIT canceled();
}

void ZVideoEncoder::ffmpegError(QProcess::ProcessError err)
{
  QString msg;
  switch (err) {
    case QProcess::FailedToStart:
      msg = "Failed to Start";
      break;
    case QProcess::Crashed:
      msg = "Crashed";
      break;
    case QProcess::Timedout:
      msg = "Timedout";
      break;
    case QProcess::WriteError:
      msg = "Write Error";
      break;
    case QProcess::ReadError:
      msg = "Read Error";
      break;
    case QProcess::UnknownError:
      msg = "Unknown Error";
      break;
    default:
      break;
  }
  Q_EMIT error(msg);
}

void ZVideoEncoder::ffmpegFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
  if (exitStatus == QProcess::NormalExit && exitCode == 0) {
    Q_EMIT finished();
  } else {
    Q_EMIT error(m_ffmpegProcess->readAllStandardError());
  }
}

void ZVideoEncoder::logStandardError()
{
  LOG(ERROR) << m_ffmpegProcess->readAllStandardError();
}

void ZVideoEncoder::logStandardOutput()
{
  LOG(INFO) << m_ffmpegProcess->readAllStandardOutput();
}

} // namespace nim
