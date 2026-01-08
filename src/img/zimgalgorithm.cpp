#include "zimgalgorithm.h"

#include "zlog.h"

#include <itkCommand.h>
#include <itkEventObject.h>
#include <itkMacro.h>
#include <itkProcessObject.h>

#include <algorithm>

namespace nim {

class ZImgAlgorithmBaseWithProgressReporter::ExternalProgressCommand final : public itk::Command
{
public:
  using Self = ExternalProgressCommand;
  using Superclass = itk::Command;
  using Pointer = itk::SmartPointer<Self>;
  itkNewMacro(Self);

  void setOwner(ZImgAlgorithmBaseWithProgressReporter* owner)
  {
    m_owner = owner;
  }

  void Execute(itk::Object* caller, const itk::EventObject& event) override
  {
    handleEvent(caller, event);
  }

  void Execute(const itk::Object* caller, const itk::EventObject& event) override
  {
    handleEvent(const_cast<itk::Object*>(caller), event);
  }

private:
  void handleEvent(itk::Object* caller, const itk::EventObject& event)
  {
    if (!m_owner) {
      return;
    }
    if (!itk::ProgressEvent().CheckEvent(&event)) {
      return;
    }
    auto* process = dynamic_cast<itk::ProcessObject*>(caller);
    if (!process) {
      return;
    }

    if (m_owner->m_cancellationToken.isCancellationRequested()) {
      if (!process->GetAbortGenerateData()) {
        process->AbortGenerateDataOn();
        LOG(INFO) << "abort external operation";
      }
      return;
    }

    m_owner->subOperationProgressChanged(std::clamp(process->GetProgress(), 0.f, 1.f), process);
  }

  ZImgAlgorithmBaseWithProgressReporter* m_owner = nullptr;
};

struct ZImgAlgorithmBaseWithProgressReporter::ExternalProgressObserverState
{
  ExternalProgressCommand::Pointer command;
};

ZImgAlgorithmBaseWithProgressReporter::ZImgAlgorithmBaseWithProgressReporter()
{
  m_externalProgressObserver = std::make_unique<ExternalProgressObserverState>();
  m_externalProgressObserver->command = ExternalProgressCommand::New();
  m_externalProgressObserver->command->setOwner(this);
}

ZImgAlgorithmBaseWithProgressReporter::~ZImgAlgorithmBaseWithProgressReporter()
{
  if (m_externalProgressObserver && m_externalProgressObserver->command) {
    m_externalProgressObserver->command->setOwner(nullptr);
  }
}

void ZImgAlgorithmBaseWithProgressReporter::setProgressReportInterval(double interval)
{
  if (interval != m_reportInterval) {
    m_reportInterval = interval;
    for (const auto& [so, wp] : m_subOperationsWeightProgress) {
      auto* sub = static_cast<ZImgAlgorithmBaseWithProgressReporter*>(so);
      sub->setProgressReportInterval(m_reportInterval / wp.weight);
    }
  }
}

void ZImgAlgorithmBaseWithProgressReporter::subOperationProgressChanged(double p, void* sender)
{
  if (auto it = m_subOperationsWeightProgress.find(sender);
      it != m_subOperationsWeightProgress.end() &&
      ((p - it->second.progress) * it->second.weight >= m_reportInterval || p == 1.0)) {
    it->second.progress = p;
    sendProgressSignal();
  }
}

void ZImgAlgorithmBaseWithProgressReporter::reportProgress(double progress)
{
  if (m_cancellationToken.isCancellationRequested()) {
    throw ZCancellationException();
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
  sender->setCancellationToken(m_cancellationToken);
  sender->setParent(this);
}

void ZImgAlgorithmBaseWithProgressReporter::registerSubOperationExternal(void* sender, double weight)
{
  CHECK(sender);
  m_subOperationsWeightProgress[sender].weight = weight;
  m_subOperationsWeightProgress[sender].progress = 0.0;

  auto* process = static_cast<itk::ProcessObject*>(sender);
  CHECK(process);
  CHECK(m_externalProgressObserver);
  CHECK(m_externalProgressObserver->command);
  process->AddObserver(itk::ProgressEvent(), m_externalProgressObserver->command);
}

void ZImgAlgorithmBaseWithProgressReporter::clearRegisteredSubOperations()
{
  m_subOperationsWeightProgress.clear();
}

void ZImgAlgorithmBaseWithProgressReporter::sendProgressSignal()
{
  double currentProgress = m_weight * m_progress;
  for (const auto& [so, wp] : m_subOperationsWeightProgress) {
    currentProgress += wp.weight * wp.progress;
  }
  if (m_parent) {
    m_parent->subOperationProgressChanged(currentProgress, this);
  } else {
    Q_EMIT progressChanged(currentProgress, this);
    Q_EMIT progressChanged(currentProgress * 100.);
  }
}

} // namespace nim
