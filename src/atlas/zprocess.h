#pragma once

#include <QProcess>

namespace nim {

class ZProcess : public QObject
{
  Q_OBJECT

public:
  explicit ZProcess(QObject* parent = nullptr);

  static void dryRun(const QString& program, const QStringList& arguments);

  void run(const QString& program, const QStringList& arguments);

  void cancel();

  bool waitForFinished(int msecs = 30000)
  {
    return m_process->waitForFinished(msecs);
  }

  bool waitForStarted(int msecs = 30000)
  {
    return m_process->waitForStarted(msecs);
  }

  [[nodiscard]] bool finishedWithoutError() const
  {
    return m_process->exitStatus() == QProcess::NormalExit && m_process->exitCode() == 0;
  }

  [[nodiscard]] auto processError() const
  {
    return m_process->readAllStandardError();
  }

Q_SIGNALS:
  void error(QString);

  void finished();

  void canceled();

protected:
  void onError(QProcess::ProcessError err);

  void onFinished(int exitCode, QProcess::ExitStatus exitStatus);

  void logStandardError();

  void logStandardOutput();

protected:
  QProcess* m_process = nullptr;
};

} // namespace nim
