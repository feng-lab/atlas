#include "zimgprocess.h"
#include "zlog.h"
#include <QThread>
#include <folly/ScopeGuard.h>

namespace nim {

ZImgProcess::ZImgProcess()
  : ZImgAlgorithmBaseWithProgressReporter()
{
}

void ZImgProcess::run()
{
  LogSinkPtr fileDestination = createFileLogSink(m_logFile);
  if (fileDestination)
    addLogSink(fileDestination);
  folly::ScopeGuard guard1 = folly::makeGuard([&fileDestination]() {
    if (fileDestination) { removeLogSink(fileDestination); }
  });
  Q_UNUSED(guard1)

  try {
    LOG(INFO) << "run " << QThread::currentThreadId();
    doWork();
    emit finished();
  }
  catch (itk::ProcessAborted const& e) {
    LOG(ERROR) << "Process Aborted by User. " << e.GetDescription();
    emit canceled();
    if (hasParent()) {
      LOG(ERROR) << "notifying parent operation..";
      throw;  // notify parent
    }
  }
  catch (itk::ExceptionObject const& excp) {
    LOG(ERROR) << "Caught itk exception: " << excp.GetDescription();
    emit processError(QString(excp.GetDescription()));
    if (hasParent()) {
      LOG(ERROR) << "notifying parent operation..";
      throw;  // notify parent
    }
  }
  catch (ZProcessAbortException const& e) {
    LOG(ERROR) << "Process Aborted by User. " << e.what();
    emit canceled();
    if (hasParent()) {
      LOG(ERROR) << "notifying parent operation..";
      throw;  // notify parent
    }
  }
  catch (ZException const& e) {
    LOG(ERROR) << "Caught exception: " << e.what();
    emit processError(e.what());
    if (hasParent()) {
      LOG(ERROR) << "notifying parent operation..";
      throw;  // notify parent
    }
  }
}

} // namespace nim
