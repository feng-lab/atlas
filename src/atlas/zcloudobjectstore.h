#pragma once

#include "zremoteobjectstore.h"

#include <QString>

#include <memory>
#include <optional>

namespace nim {

enum class ZCloudObjectProvider
{
  None,
  Gcs,
  S3,
};

struct ZCloudObjectUrl
{
  ZCloudObjectProvider provider = ZCloudObjectProvider::None;
  QString originalUrl;
  QString bucket;
  QString key;
  QString httpsUrl;
  bool preserveUrl = false;
};

[[nodiscard]] std::optional<ZCloudObjectUrl> parseCloudObjectUrl(QString url);

[[nodiscard]] QString cloudObjectUrlToHttps(QString url);

[[nodiscard]] std::shared_ptr<const ZRemoteObjectStore>
makeCloudAwareRemoteObjectStoreForUrl(QString url, std::shared_ptr<const ZRemoteObjectStore> delegate = nullptr);

} // namespace nim
