#pragma once

// base class for all img process, template parameter ReportProgress are used to
// control whether progress will be reported in compile-time
// By default progress report are turned off
// There should be no overhead because compiler should optimize
// all those empty functions call out

#include "zcpuinfo.h"
#include "zglobal.h"
#include <QObject>
#include <folly/CancellationToken.h>
#include <memory>
#include <map>
#include <set>
#include <functional>

namespace nim {

class ZImgAlgorithmBaseWithProgressReporter : public QObject
{
  Q_OBJECT

public:
  ZImgAlgorithmBaseWithProgressReporter();
  ~ZImgAlgorithmBaseWithProgressReporter() override;

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

Q_SIGNALS:
  // progress from 1 to 100, used for QProgressbar
  void progressChanged(int);

  // progress from 0.0 to 1.0
  void progressChanged(double, void* sender);

protected:
  void subOperationProgressChanged(double p, void* sender);

  // progress from 0.0 to 1.0
  void reportProgress(double progress);

  // will change the progress interval of internal operation
  void registerSubOperation(ZImgAlgorithmBaseWithProgressReporter* sender, double weight);

  // Register an external operation that reports progress via callbacks (e.g. a third-party filter).
  // `sender` must remain alive for the duration of the operation.
  void registerSubOperationExternal(void* sender, double weight);

  void clearRegisteredSubOperations();

  [[nodiscard]] bool hasParent() const
  {
    return m_parent;
  }

 private:
  class ExternalProgressCommand;
  struct ExternalProgressObserverState;

  // calculate and send signal
  void sendProgressSignal();

  std::unique_ptr<ExternalProgressObserverState> m_externalProgressObserver;

  void setParent(ZImgAlgorithmBaseWithProgressReporter* p)
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

  std::map<void*, WeightProgress> m_subOperationsWeightProgress;
  double m_weight = 1;
  double m_progress = 0;
  double m_reportInterval = 0.01;
  ZImgAlgorithmBaseWithProgressReporter* m_parent = nullptr;

  size_t m_numThreads = ZCpuInfo::instance().nLogicalCores;

  folly::CancellationToken m_cancellationToken;
};

class ZImgAlgorithmBase
{
public:
  ZImgAlgorithmBase() = default;
  ZImgAlgorithmBase(const ZImgAlgorithmBase&) = delete;
  ZImgAlgorithmBase& operator=(const ZImgAlgorithmBase&) = delete;
  ZImgAlgorithmBase(ZImgAlgorithmBase&&) = default;
  ZImgAlgorithmBase& operator=(ZImgAlgorithmBase&&) = default;

protected:
  virtual ~ZImgAlgorithmBase() = default;

  void setCancelFlag(bool*) {}

  void setProgressReportInterval(double) {}

  void setTotalSubOperationWeight(double) {}

  void reportProgress(double) {}

  // will change the progress interval of internal operation
  void registerSubOperation(void*, double) {}

  void registerSubOperationExternal(void*, double) {}

  void clearRegisteredSubOperations() {}

  void setNumberOfThreads(size_t n)
  {
    m_numThreads = n;
  }

protected:
  size_t m_numThreads = ZCpuInfo::instance().nLogicalCores;
};

template<bool ReportProgress = false>
class ZImgAlgorithm : public ZImgAlgorithmBase
{};

template<>
class ZImgAlgorithm<true> : public ZImgAlgorithmBaseWithProgressReporter
{};

} // namespace nim
