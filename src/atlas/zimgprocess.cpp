#include "zimgprocess.h"
#include "QsLog.h"
#include "QsLogDest.h"
#include <QThread>

namespace nim {

ZImgProcess::ZImgProcess()
  : ZImgAlgorithmBaseWithProgressReporter()
{
}

void ZImgProcess::run()
{
  QsLogging::DestinationPtr fileDestination;
  QsLogging::Logger& logger = QsLogging::Logger::instance();
  if (!m_logFile.isEmpty()) {
    fileDestination = QsLogging::DestinationFactory::MakeFileDestination(m_logFile);
    logger.addDestination(fileDestination);
  }
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
      if (!m_logFile.isEmpty())
        logger.removeDestination(fileDestination);
      throw;  // notify parent
    }
  }
  catch (itk::ExceptionObject const & excp) {
    LERROR() << "Caught itk exception" << excp.GetDescription();
    emit processError(QString(excp.GetDescription()));
    if (hasParent()) {
      LERROR() << "notifying parent operation..";
      if (!m_logFile.isEmpty())
        logger.removeDestination(fileDestination);
      throw;  // notify parent
    }
  }
  catch (ZProcessAbortException const & e) {
    LERROR() << "Process Aborted by User." << e.what();
    emit canceled();
    if (hasParent()) {
      LERROR() << "notifying parent operation..";
      if (!m_logFile.isEmpty())
        logger.removeDestination(fileDestination);
      throw;  // notify parent
    }
  }
  catch (ZException const & e) {
    LERROR() << "Caught exception" << e.what();
    emit processError(e.what());
    if (hasParent()) {
      LERROR() << "notifying parent operation..";
      if (!m_logFile.isEmpty())
        logger.removeDestination(fileDestination);
      throw;  // notify parent
    }
  }

  if (!m_logFile.isEmpty())
    logger.removeDestination(fileDestination);
}

} // namespace nim
