#pragma once

#include "zremoteobjectreader.h"
#include "zremoteobjectstore.h"

#include <folly/coro/Task.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nim {

// Reader-facing remote I/O boundary for one Neuroglancer source/session.
//
// A context binds a stable ZRemoteObjectStore backend to the per-reader request policy that all helpers in that
// reader should share today, primarily the timeout. Neuroglancer readers should pass this context around instead
// of threading raw transport details through every helper call. The underlying store remains available so top-level
// entry points can propagate the same backend identity to child readers and tests can still inject fake stores.
class ZNeuroglancerRemoteContext
{
public:
  ZNeuroglancerRemoteContext(std::shared_ptr<const ZRemoteObjectStore> objectStore, std::chrono::milliseconds timeout);

  [[nodiscard]] static std::shared_ptr<const ZNeuroglancerRemoteContext>
  create(std::chrono::milliseconds timeout, std::shared_ptr<const ZRemoteObjectStore> objectStore = nullptr);

  [[nodiscard]] const ZRemoteObjectStore& objectStore() const
  {
    return *m_objectStore;
  }

  [[nodiscard]] const std::shared_ptr<const ZRemoteObjectStore>& sharedObjectStore() const
  {
    return m_objectStore;
  }

  [[nodiscard]] std::chrono::milliseconds timeout() const
  {
    return m_timeout;
  }

  [[nodiscard]] folly::coro::Task<std::optional<ZHttpGetBytesResult>>
  getResponseAsync(std::string url,
                   std::vector<std::pair<std::string, std::string>> requestHeaders = {},
                   /*nullable*/ ZImgReadStatsSink* statsSink = nullptr,
                   ZImgReadStatsContext statsContext = {}) const;

  [[nodiscard]] folly::coro::Task<std::optional<std::vector<uint8_t>>>
  getBytesAsync(std::string url,
                /*nullable*/ ZImgReadStatsSink* statsSink = nullptr,
                ZImgReadStatsContext statsContext = {}) const;

  [[nodiscard]] folly::coro::Task<std::optional<std::vector<uint8_t>>>
  getRangeBytesAsync(std::string url,
                     uint64_t offset,
                     uint64_t length,
                     ZRemoteRangeReadPolicy policy = ZRemoteRangeReadPolicy::RequireExactLength,
                     /*nullable*/ ZImgReadStatsSink* statsSink = nullptr,
                     ZImgReadStatsContext statsContext = {}) const;

private:
  std::shared_ptr<const ZRemoteObjectStore> m_objectStore;
  std::chrono::milliseconds m_timeout{30000};
};

} // namespace nim
