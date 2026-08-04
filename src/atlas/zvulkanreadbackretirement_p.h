#pragma once

#include <atomic>
#include <cstdint>

namespace nim {

// Internal per-slot lifetime state for Vulkan staging readbacks. An acquisition
// becomes reusable only after both the GPU producer and mapped-data consumer
// finish. Each owner transfers or releases its notification exactly once before
// the slot can be acquired again.
class ZVulkanReadbackRetirement final
{
public:
  [[nodiscard]] bool tryAcquire() noexcept;
  [[nodiscard]] bool occupied() const noexcept;

  void notifyProducerFinished() noexcept;
  void notifyConsumerFinished() noexcept;

private:
  static constexpr uint8_t kOccupied = uint8_t{1} << 0u;
  static constexpr uint8_t kProducerFinished = uint8_t{1} << 1u;
  static constexpr uint8_t kConsumerFinished = uint8_t{1} << 2u;

  void notifyFinished(uint8_t ownerFlag) noexcept;

  std::atomic<uint8_t> m_state{0u};
};

} // namespace nim
