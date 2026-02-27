#pragma once

#include "zcpuinfo.h"
#include "zglobal.h"

#include <cstddef>
#include <folly/CancellationToken.h>
#include <functional>
#include <map>
#include <memory>

namespace nim {

// Base class for image algorithms that may report progress and support cancellation.
//
// Design goals:
// - Keep the algorithm-facing API small (reportProgress + optional sub-operation tracking).
// - Keep overhead low when neither progress nor cancellation is used.
// - Avoid QObject coupling so algorithms can run on any executor/thread.
class ZImgAlgorithm
{
public:
  ZImgAlgorithm();
  virtual ~ZImgAlgorithm();

  // default report 1 percent change
  // larger value can reduce the number of signals
  void setProgressReportInterval(double interval = 0.01);

  // among all weight, how many percent are contributed by suboperation, should between 0.0 to 1.0
  // default is 0
  // the number should match the summation of all registered sub operation weight
  // The reason for this function is that sub operation can be registered in any stage of the
  // algorithm, but we must know it before the algorithm start to calculate correct progress
  // change this in the middle of operation will cause a little bump
  void setTotalSubOperationWeight(double w)
  {
    m_weight = 1 - w;
  }

  // If cancelled, current algorithm will abort and throw a ZCancellationException.
  void setCancellationToken(const folly::CancellationToken& token)
  {
    m_cancellationToken = token;
  }

  // Progress callback from 0.0 to 1.0. This is invoked only for the top-level operation.
  // Sub-operations report progress through their parent via registerSubOperation().
  void setProgressCallback(std::function<void(double)> cb)
  {
    m_progressCallback = std::move(cb);
  }

protected:
  // progress from 0.0 to 1.0
  void reportProgress(double progress);

  // will change the progress interval of internal operation
  void registerSubOperation(ZImgAlgorithm* sender, double weight);

  // Register an external operation that reports progress via callbacks (e.g. a third-party filter).
  // `sender` must remain alive for the duration of the operation.
  void registerSubOperationExternal(void* sender, double weight);

  void clearRegisteredSubOperations();

  [[nodiscard]] bool hasParent() const
  {
    return m_parent != nullptr;
  }

 private:
  class ExternalProgressCommand;
  struct ExternalProgressObserverState;

  void subOperationProgressChanged(double p, ZImgAlgorithm* sender);
  void externalOperationProgressChanged(double p, void* sender);

  // calculate and send signal
  void sendProgressSignal();

  std::unique_ptr<ExternalProgressObserverState> m_externalProgressObserver;

  void setParent(ZImgAlgorithm* p)
  {
    m_parent = p;
  }

  void setNumberOfThreads(size_t n)
  {
    m_numThreads = n;
  }

protected:
  struct WeightProgress
  {
    double weight;
    double progress;
  };

  std::map<ZImgAlgorithm*, WeightProgress> m_subOperationsWeightProgress;
  std::map<void*, WeightProgress> m_externalOperationsWeightProgress;
  double m_weight = 1;
  double m_progress = 0;
  double m_reportInterval = 0.01;
  ZImgAlgorithm* m_parent = nullptr;

  size_t m_numThreads = ZCpuInfo::instance().nLogicalCores;

  folly::CancellationToken m_cancellationToken;
  std::function<void(double)> m_progressCallback;
};

} // namespace nim
