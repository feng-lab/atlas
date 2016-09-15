#pragma once

// base class for all img process, template parameter ReportProgress are used to
// control whether progress will be reported in compile-time
// By default progress report are turned off
// There should be no overhead because compiler should optimize
// all those empty functions call out

#include "zexception.h"
#include <itkCommand.h>
#include <itkProcessObject.h>
#include <QObject>
#include <map>
#include <set>

#undef _WIN32_WINNT

namespace nim {

class ZImgAlgorithmBaseWithProgressReporter : public QObject
{
Q_OBJECT
public:
  ZImgAlgorithmBaseWithProgressReporter();

  // if flag is set to true, current algorithm will abort and throw a ZProcessAbortException
  // or itk::ProcessAborted
  inline void setCancelFlag(std::atomic<bool>* flag)
  { m_cancelFlag = flag; }

  // default report 1 percent change
  // larger value can reduce the number of signals
  void setProgressReportInterval(double interval = 0.01);

  // among all weight, how many percent are contributed by suboperation, should between 0.0 to 1.0
  // default is 0
  // the number should match the summation of all registered sub operation weight
  // The reason for this function is that sub operation can be registered in any stage of the
  // algorithm, but we must know it before the algorithm start to calculate correct progress
  // change this in the middle of operation will cause a little bump
  inline void setTotalSubOperationWeight(double w)
  { m_weight = 1 - w; }

signals:

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

  void registerSubOperation(itk::ProcessObject* filter, double weight);

  void clearRegisteredSubOperations();

  inline bool hasParent() const
  { return m_parent; }

private:
  // calculate and send signal
  void sendProgressSignal();

  inline double clamp(double progress, double min = 0.0, double max = 1.0)
  { return std::max(min, std::min(progress, max)); }

  using CommandType = itk::MemberCommand<ZImgAlgorithmBaseWithProgressReporter>;
  using CommandPointer = CommandType::Pointer;

  // call back function for ITK
  void processITKEvent(itk::Object* caller, const itk::EventObject& event);

  void constProcessITKEvent(const itk::Object* caller, const itk::EventObject& event);

  CommandPointer m_CallbackCommand;

  inline void setParent(ZImgAlgorithmBaseWithProgressReporter* p)
  { m_parent = p; }

protected:
  struct WeightProgress
  {
    double weight;
    double progress;
  };

  std::map<void*, WeightProgress> m_subOperationsWeightProgress;
  std::set<itk::ProcessObject*> m_itkOperations;
  double m_weight;
  double m_progress;
  double m_reportInterval;
  std::atomic<bool>* m_cancelFlag;
  ZImgAlgorithmBaseWithProgressReporter* m_parent;
};

class ZImgAlgorithmBase
{
public:

protected:
  virtual ~ZImgAlgorithmBase() = default;

  inline void setCancelFlag(bool* /*unused*/)
  {}

  inline void setProgressReportInterval(double /*unused*/)
  {}

  inline void setTotalSubOperationWeight(double /*unused*/)
  {}

  inline void reportProgress(double /*unused*/)
  {}

  // will change the progress interval of internal operation
  inline void registerSubOperation(void* /*unused*/, double /*unused*/)
  {}

  inline void registerSubOperation(itk::ProcessObject* /*unused*/, double /*unused*/)
  {}

  inline void clearRegisteredSubOperations()
  {}
};

template<bool ReportProgress = false>
class ZImgAlgorithm : public ZImgAlgorithmBase
{
};

template<>
class ZImgAlgorithm<true> : public ZImgAlgorithmBaseWithProgressReporter
{
};

} // namespace nim

