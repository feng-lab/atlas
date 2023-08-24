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

  bool waitForFinished(int msecs = 3000)
  {
    return m_process->waitForFinished(msecs);
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
