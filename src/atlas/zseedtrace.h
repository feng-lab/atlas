#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

class QString;

namespace nim {

class ZDoc;
class ZImgPack;
class ZSwc;

void startSeedTraceInteractive(ZDoc& doc,
                               const QString& actionName,
                               size_t sourceImgObjId,
                               const std::shared_ptr<ZImgPack>& imgPack,
                               size_t sc,
                               size_t t,
                               std::array<double, 3> seed,
                               const QString& traceConfigPath,
                               std::optional<std::pair<size_t, ZSwc>> hostSwcOpt,
                               bool promoteNewSwcToExistingTarget,
                               std::function<void(size_t newSwcId)> onNewSwcCreated = {});

} // namespace nim
