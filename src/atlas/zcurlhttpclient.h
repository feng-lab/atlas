#pragma once

#include "zhttpclient.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nim {

class ZHttpDiskCache;

class ZCurlHttpClient
{
public:
  static ZCurlHttpClient& instance();

  // Returns std::nullopt for missing resources. For Neuroglancer parity, both
  // HTTP 403 and 404 are normalized to "not found" because some object stores
  // report absent objects as 403.
  folly::coro::Task<std::optional<ZHttpGetBytesResult>>
  getBytes(std::string url,
           std::chrono::milliseconds timeout,
           std::vector<std::pair<std::string, std::string>> requestHeaders = {});

private:
  ZCurlHttpClient();
  ~ZCurlHttpClient();

private:
  std::string m_caBundlePath;
  std::string m_trustSourceDescription;
  std::unique_ptr<ZHttpDiskCache> m_diskCache;
};

} // namespace nim
