#pragma once

#include <optional>

#include <QString>

namespace nim {

[[nodiscard]] std::optional<QString> decodeSupportedNeuroglancerPrecomputedSourceUrl(QString url);

[[nodiscard]] QString normalizeNeuroglancerUrlDropFragment(QString url);

[[nodiscard]] QString mapCloudStorageUrlToHttps(QString url);

[[nodiscard]] QString normalizeNeuroglancerPrecomputedRootUrl(QString url);

} // namespace nim
