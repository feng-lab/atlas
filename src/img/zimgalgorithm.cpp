#include "zimgalgorithm.h"

#include "zlog.h"

#include <itkCommand.h>
#include <itkEventObject.h>
#include <itkMacro.h>
#include <itkProcessObject.h>

#include <algorithm>

namespace nim {

class ZImgAlgorithm::ExternalProgressCommand final : public itk::Command
{
public:
  using Self = ExternalProgressCommand;
  using Superclass = itk::Command;
  using Pointer = itk::SmartPointer<Self>;
  itkNewMacro(Self);

  void setOwner(ZImgAlgorithm* owner)
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

    m_owner->externalOperationProgressChanged(std::clamp(process->GetProgress(), 0.f, 1.f), process);
  }

  ZImgAlgorithm* m_owner = nullptr;
};

struct ZImgAlgorithm::ExternalProgressObserverState
{
  ExternalProgressCommand::Pointer command;
};

ZImgAlgorithm::ZImgAlgorithm() = default;

ZImgAlgorithm::~ZImgAlgorithm()
{
  if (m_externalProgressObserver && m_externalProgressObserver->command) {
    m_externalProgressObserver->command->setOwner(nullptr);
  }
}

void ZImgAlgorithm::setProgressReportInterval(double interval)
{
  if (interval != m_reportInterval) {
    m_reportInterval = interval;
    for (const auto& [sub, wp] : m_subOperationsWeightProgress) {
      CHECK(sub);
      CHECK(wp.weight > 0.0);
      sub->setProgressReportInterval(m_reportInterval / wp.weight);
    }
  }
}

void ZImgAlgorithm::subOperationProgressChanged(double p, ZImgAlgorithm* sender)
{
  CHECK(sender);
  if (auto it = m_subOperationsWeightProgress.find(sender); it != m_subOperationsWeightProgress.end()) {
    if (((p - it->second.progress) * it->second.weight >= m_reportInterval) || p == 1.0) {
      it->second.progress = p;
      sendProgressSignal();
    }
  }
}

void ZImgAlgorithm::externalOperationProgressChanged(double p, void* sender)
{
  CHECK(sender);
  if (auto it = m_externalOperationsWeightProgress.find(sender); it != m_externalOperationsWeightProgress.end()) {
    if (((p - it->second.progress) * it->second.weight >= m_reportInterval) || p == 1.0) {
      it->second.progress = p;
      sendProgressSignal();
    }
  }
}

void ZImgAlgorithm::reportProgress(double progress)
{
  if (m_cancellationToken.canBeCancelled()) {
    if (m_cancellationToken.isCancellationRequested()) {
      throw ZCancellationException();
    }
  } else if (!m_parent && !m_progressCallback) {
    return;
  }

  if (!m_parent && !m_progressCallback) {
    return;
  }
  if ((progress - m_progress) * m_weight >= m_reportInterval || progress == 1.0) {
    m_progress = progress;
    sendProgressSignal();
  }
}

void ZImgAlgorithm::registerSubOperation(ZImgAlgorithm* sender, double weight)
{
  CHECK(sender);
  CHECK(weight > 0.0);
  m_subOperationsWeightProgress[sender].weight = weight;
  m_subOperationsWeightProgress[sender].progress = 0.0;
  sender->setProgressReportInterval(m_reportInterval / weight);
  sender->setCancellationToken(m_cancellationToken);
  sender->setParent(this);
}

void ZImgAlgorithm::registerSubOperationExternal(void* sender, double weight)
{
  CHECK(sender);
  CHECK(weight > 0.0);

  auto [it, inserted] = m_externalOperationsWeightProgress.emplace(sender, WeightProgress{weight, 0.0});
  if (!inserted) {
    it->second.weight = weight;
    it->second.progress = 0.0;
  }

  auto* process = static_cast<itk::ProcessObject*>(sender);
  CHECK(process);
  if (!m_externalProgressObserver) {
    m_externalProgressObserver = std::make_unique<ExternalProgressObserverState>();
    m_externalProgressObserver->command = ExternalProgressCommand::New();
    m_externalProgressObserver->command->setOwner(this);
  }
  CHECK(m_externalProgressObserver->command);

  if (inserted) {
    process->AddObserver(itk::ProgressEvent(), m_externalProgressObserver->command);
  }
}

void ZImgAlgorithm::clearRegisteredSubOperations()
{
  m_subOperationsWeightProgress.clear();
  m_externalOperationsWeightProgress.clear();
}

void ZImgAlgorithm::sendProgressSignal()
{
  if (!m_parent && !m_progressCallback) {
    return;
  }

  double currentProgress = m_weight * m_progress;
  for (const auto& [so, wp] : m_subOperationsWeightProgress) {
    currentProgress += wp.weight * wp.progress;
  }
  for (const auto& [so, wp] : m_externalOperationsWeightProgress) {
    currentProgress += wp.weight * wp.progress;
  }
  if (m_parent) {
    m_parent->subOperationProgressChanged(currentProgress, this);
  } else {
    m_progressCallback(currentProgress);
  }
}

} // namespace nim
