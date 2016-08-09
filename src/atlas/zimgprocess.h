#pragma once

#include "zimgalgorithm.h"

namespace nim {

class ZImgProcess : public ZImgAlgorithmBaseWithProgressReporter
{
Q_OBJECT
public:
  ZImgProcess();

  // log output
  void setLogFile(const QString& logFile)
  { m_logFile = logFile; }

  void run();

signals:

  void canceled();

  void processError(QString);

  void finished();

protected:
  virtual void doWork() = 0;

private:
  QString m_logFile;
};

} // namespace nim

