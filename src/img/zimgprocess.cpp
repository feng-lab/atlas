#include "zimgprocess.h"

#include "zlog.h"
#include <QThread>
#include <folly/ScopeGuard.h>

namespace nim {

void ZImgProcess::run()
{
  LogSinkPtr fileDestination = createFileLogSink(m_logFile);
  if (fileDestination) {
    addLogSink(fileDestination);
  }
  auto guard1 = folly::makeGuard([&fileDestination]() {
    if (fileDestination) {
      removeLogSink(fileDestination);
    }
  });

  try {
    LOG(INFO) << "run " << QThread::currentThreadId();
    doWork();
    Q_EMIT finished();
  }
  catch (itk::ProcessAborted const& e) {
    QString errMsg = QString("Cancelled by user: %1").arg(e.what());
    LOG(ERROR) << errMsg;
    Q_EMIT processError(errMsg);
    if (hasParent()) {
      LOG(ERROR) << "notifying parent operation..";
      throw; // notify parent
    }
  }
  catch (itk::ExceptionObject const& e) {
    QString errMsg = QString("Caught itk exception: %1").arg(e.what());
    LOG(ERROR) << errMsg;
    Q_EMIT processError(errMsg);
    if (hasParent()) {
      LOG(ERROR) << "notifying parent operation..";
      throw; // notify parent
    }
  }
  catch (ZException const& e) {
    QString errMsg = QString("Caught exception: %1").arg(e.what());
    LOG(ERROR) << errMsg;
    Q_EMIT processError(errMsg);
    if (hasParent()) {
      LOG(ERROR) << "notifying parent operation..";
      throw; // notify parent
    }
  }
}

void ZImgProcess::runInPython()
{
  LogSinkPtr fileDestination = createFileLogSink(m_logFile);
  if (fileDestination) {
    addLogSink(fileDestination);
  }
  auto guard1 = folly::makeGuard([&fileDestination]() {
    if (fileDestination) {
      removeLogSink(fileDestination);
    }
  });

  try {
    LOG(INFO) << "run " << QThread::currentThreadId();
    doWork();
    Q_EMIT finished();
  }
  catch (itk::ProcessAborted const& e) {
    LOG(ERROR) << "Process Aborted by User. " << e.what();
    if (hasParent()) {
      LOG(ERROR) << "notifying parent operation..";
    }
    throw ZException(e.what());
  }
  catch (itk::ExceptionObject const& excp) {
    LOG(ERROR) << "Caught itk exception: " << excp.what();
    if (hasParent()) {
      LOG(ERROR) << "notifying parent operation..";
    }
    throw ZException(excp.what());
  }
  catch (ZException const& e) {
    LOG(ERROR) << "Caught exception: " << e.what();
    if (hasParent()) {
      LOG(ERROR) << "notifying parent operation..";
    }
    throw;
  }
}

} // namespace nim
