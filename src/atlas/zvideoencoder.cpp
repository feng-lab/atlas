#include "zvideoencoder.h"

#include "zlog.h"
#include "zsysteminfo.h"

namespace nim {

ZVideoEncoder::ZVideoEncoder(QObject* parent)
  : ZProcess(parent, true)
{}

std::tuple<QString, QStringList> ZVideoEncoder::encodeDryRun(const QDir& dir,
                                                             const QString& namePrefix,
                                                             int fieldWidth,
                                                             int framesPerSecond,
                                                             const QString& outputFilename)
{
#ifdef _WIN32
  QString program = ZSystemInfo::resourcesDir().absoluteFilePath("ffmpeg.exe");
#else
  QString program = ZSystemInfo::resourcesDir().absoluteFilePath("ffmpeg");
#endif
  QStringList arguments;
  arguments << "-r" << QString::number(framesPerSecond) << "-i"
            << (QString("%1/%2% 0%3d.png").arg(dir.absolutePath()).arg(namePrefix).arg(fieldWidth).replace("% 0", "%0"))
            << "-c:v"
            << "libx264"
            << "-crf"
            << "18"
            << "-pix_fmt"
            << "yuv420p"
            << "-r" << QString::number(framesPerSecond) << outputFilename;
  LOG(INFO) << program << " " << arguments.join(" ");
  return std::make_tuple(program, arguments);
}

void ZVideoEncoder::encode(const QDir& dir,
                           const QString& namePrefix,
                           int fieldWidth,
                           int framesPerSecond,
                           const QString& outputFilename)
{
  if (m_process->state() != QProcess::NotRunning) {
    Q_EMIT error("Encoder is already running.");
    return;
  }

  const auto& [program, arguments] = encodeDryRun(dir, namePrefix, fieldWidth, framesPerSecond, outputFilename);
  m_process->start(program, arguments);
}

} // namespace nim
