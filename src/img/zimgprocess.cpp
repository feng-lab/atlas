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
    LOG(ERROR) << "Process Aborted by User. " << e.what();
    Q_EMIT canceled();
    if (hasParent()) {
      LOG(ERROR) << "notifying parent operation..";
      throw; // notify parent
    }
  }
  catch (itk::ExceptionObject const& excp) {
    LOG(ERROR) << "Caught itk exception: " << excp.what();
    Q_EMIT processError(QString(excp.what()));
    if (hasParent()) {
      LOG(ERROR) << "notifying parent operation..";
      throw; // notify parent
    }
  }
  catch (ZProcessAbortException const& e) {
    LOG(ERROR) << "Process Aborted by User. " << e.what();
    Q_EMIT canceled();
    if (hasParent()) {
      LOG(ERROR) << "notifying parent operation..";
      throw; // notify parent
    }
  }
  catch (ZException const& e) {
    LOG(ERROR) << "Caught exception: " << e.what();
    Q_EMIT processError(e.what());
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
  catch (ZProcessAbortException const& e) {
    LOG(ERROR) << "Process Aborted by User. " << e.what();
    if (hasParent()) {
      LOG(ERROR) << "notifying parent operation..";
    }
    throw;
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
