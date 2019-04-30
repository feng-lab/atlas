#pragma once

#include <QString>

namespace nim {

void
initImgLib(const char* argv0, const QString& jdkDIR = "", const QString& jarsDIR = "", const QString& logFilename = "");

void shutdownImgLib();

} // namespace nim

