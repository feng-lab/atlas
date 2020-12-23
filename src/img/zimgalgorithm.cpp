#include "zimgalgorithm.h"

#include "zlog.h"

namespace nim {

ZImgAlgorithmBaseWithProgressReporter::ZImgAlgorithmBaseWithProgressReporter()
{
  m_CallbackCommand = CommandType::New();
  m_CallbackCommand->SetCallbackFunction(this, &ZImgAlgorithmBaseWithProgressReporter::processITKEvent);
  m_CallbackCommand->SetCallbackFunction(this, &ZImgAlgorithmBaseWithProgressReporter::constProcessITKEvent);
}

void ZImgAlgorithmBaseWithProgressReporter::setProgressReportInterval(double interval)
{
  if (interval != m_reportInterval) {
    m_reportInterval = interval;
    for (const auto& soWP : m_subOperationsWeightProgress) {
      auto* sub = static_cast<ZImgAlgorithmBaseWithProgressReporter*>(soWP.first);
      sub->setProgressReportInterval(m_reportInterval / soWP.second.weight);
    }
  }
}

void ZImgAlgorithmBaseWithProgressReporter::subOperationProgressChanged(double p, void* sender)
{
  if (false && isCancelledFun && isCancelledFun()) {
    auto it = m_itkOperations.find(static_cast<itk::ProcessObject*>(sender));
    if (it != m_itkOperations.end()) {
      (*it)->AbortGenerateDataOn();
    } else {
      throw ZProcessAbortException("");
    }
  } else {
    auto it = m_subOperationsWeightProgress.find(sender);
    if (it != m_subOperationsWeightProgress.end() &&
        ((p - it->second.progress) * it->second.weight >= m_reportInterval || p == 1.0)) {
      it->second.progress = p;
      sendProgressSignal();
    }
  }
}

void ZImgAlgorithmBaseWithProgressReporter::reportProgress(double progress)
{
  if (isCancelledFun && isCancelledFun()) {
    throw ZProcessAbortException("");
  }
  if ((progress - m_progress) * m_weight >= m_reportInterval || progress == 1.0) {
    m_progress = progress;
    sendProgressSignal();
  }
}

void ZImgAlgorithmBaseWithProgressReporter::registerSubOperation(ZImgAlgorithmBaseWithProgressReporter* sender,
                                                                 double weight)
{
  m_subOperationsWeightProgress[sender].weight = weight;
  m_subOperationsWeightProgress[sender].progress = 0.0;
  sender->setProgressReportInterval(m_reportInterval / weight);
  sender->isCancelledFun = isCancelledFun;
  sender->setParent(this);
}

void ZImgAlgorithmBaseWithProgressReporter::registerSubOperation(itk::ProcessObject* filter, double weight)
{
  m_subOperationsWeightProgress[filter].weight = weight;
  m_subOperationsWeightProgress[filter].progress = 0.0;
  m_itkOperations.insert(filter);
  filter->AddObserver(itk::ProgressEvent(), m_CallbackCommand);
}

void ZImgAlgorithmBaseWithProgressReporter::clearRegisteredSubOperations()
{
  m_itkOperations.clear();
  m_subOperationsWeightProgress.clear();
}

void ZImgAlgorithmBaseWithProgressReporter::sendProgressSignal()
{
  double currentProgress = m_weight * m_progress;
  for (const auto& soWP : m_subOperationsWeightProgress) {
    currentProgress += soWP.second.weight * soWP.second.progress;
  }
  if (m_parent) {
    m_parent->subOperationProgressChanged(currentProgress, this);
  } else {
    emit progressChanged(currentProgress, this);
    emit progressChanged(currentProgress * 100.);
  }
}

void ZImgAlgorithmBaseWithProgressReporter::processITKEvent(itk::Object* caller, const itk::EventObject& event)
{
  if (itk::ProgressEvent().CheckEvent(&event)) {
    if (auto process = dynamic_cast<itk::ProcessObject*>(caller)) {
      if (isCancelledFun && isCancelledFun()) {
        if (!process->GetAbortGenerateData()) {
          process->AbortGenerateDataOn();
          LOG(INFO) << "abort itk 1";
        }
      } else {
        subOperationProgressChanged(std::clamp(process->GetProgress(), 0.f, 1.f), process);
      }
    }
  }
}

void
ZImgAlgorithmBaseWithProgressReporter::constProcessITKEvent(const itk::Object* caller, const itk::EventObject& event)
{
  if (itk::ProgressEvent().CheckEvent(&event)) {
    if (auto process = const_cast<itk::ProcessObject*>(dynamic_cast<const itk::ProcessObject*>(caller))) {
      if (isCancelledFun && isCancelledFun()) {
        if (!process->GetAbortGenerateData()) {
          process->AbortGenerateDataOn();
          LOG(INFO) << "abort itk 2";
        }
      } else {
        subOperationProgressChanged(std::clamp(process->GetProgress(), 0.f, 1.f), process);
      }
    }
  }
}

} // namespace nim
