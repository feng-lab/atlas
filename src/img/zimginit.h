#pragma once

#include "zexception.h"

namespace nim {

void initImgLib(const char* argv0,
                const QString& resourcesDIR = "",
                const QString& jreDIR = "",
                const QString& jarsDIR = "",
                const QString& logFilename = "",
                bool isApp = true,
                bool isGUIMode = true);

void shutdownImgLib();

} // namespace nim
