#include "zprocess.h"

#include "zlog.h"

namespace nim {

ZProcess::ZProcess(QObject* parent)
  : QObject(parent)
{
  m_process = new QProcess(this);
  connect(m_process, &QProcess::errorOccurred, this, &ZProcess::onError);
  connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &ZProcess::onFinished);
  connect(m_process, &QProcess::readyReadStandardOutput, this, &ZProcess::logStandardOutput);
  connect(m_process, &QProcess::readyReadStandardError, this, &ZProcess::logStandardError);
}

void ZProcess::dryRun(const QString& program, const QStringList& arguments)
{
  LOG(INFO) << program << " " << arguments.join(" ");
}

void ZProcess::run(const QString& program, const QStringList& arguments)
{
  if (m_process->state() != QProcess::NotRunning) {
    Q_EMIT error("Process is already running.");
    return;
  }

  dryRun(program, arguments);
  m_process->start(program, arguments);
}

void ZProcess::cancel()
{
  blockSignals(true);
  m_process->kill();
  m_process->waitForFinished();
  blockSignals(false);
  Q_EMIT canceled();
}

void ZProcess::onError(QProcess::ProcessError err)
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

void ZProcess::onFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
  if (exitStatus == QProcess::NormalExit && exitCode == 0) {
    Q_EMIT finished();
  } else {
    Q_EMIT error(m_process->readAllStandardError());
  }
}

void ZProcess::logStandardError()
{
  LOG(ERROR) << m_process->readAllStandardError();
}

void ZProcess::logStandardOutput()
{
  LOG(INFO) << m_process->readAllStandardOutput();
}

} // namespace nim
