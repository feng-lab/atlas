#pragma once

#include <QString>

namespace nim {

// neuTube 2.0 CLI runner (migration target).
//
// This is a thin compatibility layer that backs Atlas' `--command` CLI mode.
class ZRunNeuTuCommand2
{
public:
  int run(int argc, char* argv[]);
  int run(int argc, char* argv[], const QString& jsonDirPath);
};

} // namespace nim
