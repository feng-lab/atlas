#pragma once

#include "zhttpclient.h"

#include <folly/coro/Task.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nim {

// Lowest-level remote-read seam for Atlas.
//
// This interface intentionally stays generic, but it does preserve one transport-level concept explicitly:
// exact byte-range requests. Reader code should usually depend on ZNeuroglancerRemoteContext instead; this store
// exists so the transport/backend can be swapped or faked in tests without pulling the HTTP runtime into every reader.
class ZRemoteObjectStore
{
public:
  virtual ~ZRemoteObjectStore() = default;

  [[nodiscard]] virtual folly::coro::Task<std::optional<ZHttpGetBytesResult>>
  getBytes(ZHttpGetRequest request) const = 0;

  // Cache-scope token for any metadata derived from remote object content.
  //
  // Stores are allowed to expose different bytes for the same URL when backend/auth/session state differs.
  // Callers that cache parsed remote content above this layer must therefore key by this scope token as well
  // as by URL. Stores that intentionally expose identical content may override this and return the same token.
  [[nodiscard]] virtual const void* contentCacheScopeToken() const
  {
    return this;
  }

  [[nodiscard]] static const ZRemoteObjectStore& defaultStore();
  [[nodiscard]] static std::shared_ptr<const ZRemoteObjectStore> sharedDefaultStore();
};

class ZHttpRemoteObjectStore final : public ZRemoteObjectStore
{
public:
  [[nodiscard]] folly::coro::Task<std::optional<ZHttpGetBytesResult>> getBytes(ZHttpGetRequest request) const override;

  [[nodiscard]] const void* contentCacheScopeToken() const override;

  [[nodiscard]] static std::shared_ptr<const ZRemoteObjectStore> sharedInstance();
};

} // namespace nim
