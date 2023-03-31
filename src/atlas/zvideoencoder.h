#pragma once

#include <QObject>
#include <QProcess>
#include <QDir>

namespace nim {

class ZVideoEncoder : public QObject
{
  Q_OBJECT

public:
  explicit ZVideoEncoder(QObject* parent = nullptr);

  static std::tuple<QString, QStringList> encodeDryRun(const QDir& dir,
                                                       const QString& namePrefix,
                                                       int fieldWidth,
                                                       int framesPerSecond,
                                                       const QString& outputFilename);

  void encode(const QDir& dir,
              const QString& namePrefix,
              int fieldWidth,
              int framesPerSecond,
              const QString& outputFilename);

  void cancel();

  bool waitForFinished(int msecs = 3000)
  {
    return m_ffmpegProcess->waitForFinished(msecs);
  }

Q_SIGNALS:

  void error(QString);

  void finished();

  void canceled();

protected:
  void ffmpegError(QProcess::ProcessError err);

  void ffmpegFinished(int exitCode, QProcess::ExitStatus exitStatus);

  void logStandardError();

  void logStandardOutput();

private:
  QProcess* m_ffmpegProcess;
};

} // namespace nim
