#pragma once

#include <QObject>
#include <QString>
#include <QProcess>
#include <QDir>

namespace nim {

class ZVideoEncoder : public QObject
{
Q_OBJECT
public:
  explicit ZVideoEncoder(QObject* parent = nullptr);

  void encode(const QDir& dir, const QString& namePrefix, int fieldWidth,
              int framesPerSecond, const QString& outputFilename);

  void cancel();

signals:

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

  bool m_lock = false;

  QDir m_tmpDir;
};

} // namespace nim

