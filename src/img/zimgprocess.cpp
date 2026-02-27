#include "zimgprocess.h"

#include "zexception.h"
#include "zlog.h"
#include <itkMacro.h>
#include <itkProcessObject.h>
#include <folly/ScopeGuard.h>

namespace nim {

void ZImgProcess::run()
{
  auto fileDestination = createFileLogSink(m_logFile);
  if (fileDestination) {
    addLogSink(fileDestination);
  }
  auto guard1 = folly::makeGuard([&fileDestination]() {
    if (fileDestination) {
      removeLogSink(fileDestination);
    }
  });

  try {
    doWork();
  }
  catch (const itk::ProcessAborted& e) {
    LOG(ERROR) << "Cancelled by user: " << e.what();
    throw ZCancellationException(QString("Cancelled by user: %1").arg(e.what()));
  }
  catch (const itk::ExceptionObject& e) {
    LOG(ERROR) << "Caught itk exception: " << e.what();
    throw ZException(QString("Caught itk exception: %1").arg(e.what()));
  }
  catch (const ZCancellationException&) {
    LOG(ERROR) << "Cancelled by user";
    throw;
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Caught ZException: " << e.what();
    throw;
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Caught std exception: " << e.what();
    throw ZException(QString("Caught std exception: %1").arg(e.what()));
  }
}

} // namespace nim
