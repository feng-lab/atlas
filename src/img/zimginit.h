#pragma once

#include "zexception.h"

namespace nim {

void initImgLib(const char* argv0,
                const QString& resourcesDIR = "",
                const QString& jdkDIR = "",
                const QString& jarsDIR = "",
                const QString& logFilename = "",
                bool isApp = true,
                bool isGUIMode = true);

void shutdownImgLib(bool isApp = true);

} // namespace nim
