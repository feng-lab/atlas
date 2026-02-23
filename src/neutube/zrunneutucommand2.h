#pragma once

#include <string_view>

namespace nim {

// neuTube 2.0 CLI runner (migration target).
//
// For now, this is a thin compatibility layer so we can introduce a stable
// `--command2` entrypoint in Atlas while we migrate functionality incrementally.
class ZRunNeuTuCommand2
{
public:
  int run(int argc, char* argv[]);
  int run(int argc, char* argv[], std::string_view jsonDirPath);
};

} // namespace nim
