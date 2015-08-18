#ifndef ZIMGPROCESS_H
#define ZIMGPROCESS_H

#include "zimgalgorithm.h"

namespace nim {

class ZImgProcess : public ZImgAlgorithmBaseWithProgressReporter
{
  Q_OBJECT
public:
  ZImgProcess();

  // log output
  void setLogFile(const QString &logFile) { m_logFile = logFile; }

signals:
  void canceled();
  void processError(QString);
  void finished();

public slots:
  void run();

protected:
  virtual void doWork() = 0;

private:
  QString m_logFile;
};

} // namespace nim

#endif // ZIMGPROCESS_H
