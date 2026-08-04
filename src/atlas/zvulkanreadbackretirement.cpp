#include "zvulkanreadbackretirement_p.h"

#include "zlog.h"

namespace nim {

bool ZVulkanReadbackRetirement::tryAcquire() noexcept
{
  uint8_t expected = 0u;
  return m_state.compare_exchange_strong(expected, kOccupied, std::memory_order_acq_rel, std::memory_order_acquire);
}

bool ZVulkanReadbackRetirement::occupied() const noexcept
{
  return (m_state.load(std::memory_order_acquire) & kOccupied) != 0u;
}

void ZVulkanReadbackRetirement::notifyProducerFinished() noexcept
{
  notifyFinished(kProducerFinished);
}

void ZVulkanReadbackRetirement::notifyConsumerFinished() noexcept
{
  notifyFinished(kConsumerFinished);
}

void ZVulkanReadbackRetirement::notifyFinished(uint8_t ownerFlag) noexcept
{
  uint8_t observed = m_state.load(std::memory_order_acquire);
  for (;;) {
    CHECK_NE(observed & kOccupied, 0u) << "Readback owner finished an unoccupied staging slot";
    CHECK_EQ(observed & ownerFlag, 0u) << "Readback owner finished the same staging acquisition twice";

    const uint8_t transitioned = observed | ownerFlag;
    const uint8_t finishedMask = kProducerFinished | kConsumerFinished;
    const uint8_t desired = (transitioned & finishedMask) == finishedMask ? 0u : transitioned;
    if (m_state.compare_exchange_weak(observed, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
      return;
    }
  }
}

} // namespace nim
