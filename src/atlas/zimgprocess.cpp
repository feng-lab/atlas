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
  LogSinkPtr fileDestination = addFileLogSink(m_logFile);
  folly::ScopeGuard guard1 = folly::makeGuard([&fileDestination]() {
    if (fileDestination) removeLogSink(fileDestination);
  });
  Q_UNUSED(guard1)

  try {
    LDEBUG() << "run " << QThread::currentThreadId();
    doWork();
    emit finished();
  }
  catch (itk::ProcessAborted const & e) {
    LERROR() << "Process Aborted by User." << e.GetDescription();
    emit canceled();
    if (hasParent()) {
      LERROR() << "notifying parent operation..";
      throw;  // notify parent
    }
  }
  catch (itk::ExceptionObject const & excp) {
    LERROR() << "Caught itk exception" << excp.GetDescription();
    emit processError(QString(excp.GetDescription()));
    if (hasParent()) {
      LERROR() << "notifying parent operation..";
      throw;  // notify parent
    }
  }
  catch (ZProcessAbortException const & e) {
    LERROR() << "Process Aborted by User." << e.what();
    emit canceled();
    if (hasParent()) {
      LERROR() << "notifying parent operation..";
      throw;  // notify parent
    }
  }
  catch (ZException const & e) {
    LERROR() << "Caught exception" << e.what();
    emit processError(e.what());
    if (hasParent()) {
      LERROR() << "notifying parent operation..";
      throw;  // notify parent
    }
  }
}

} // namespace nim
