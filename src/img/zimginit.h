#pragma once

#include "zexception.h"

namespace nim {

void initImgLib(const QString& resourcesDIR = "",
                const QString& jreDIR = "",
                const QString& jarsDIR = "",
                bool isApp = false,
                bool isGUIMode = false);

void shutdownImgLib();

} // namespace nim
