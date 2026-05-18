#pragma once

#include <absl/flags/declare.h>
#include <absl/flags/flag.h>
#include <absl/flags/parse.h>
#include <absl/flags/reflection.h>
#include <absl/flags/usage.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nim {

struct ZCommandLineFlagInfo
{
  std::string name;
  std::string type;
  std::string description;
  std::string defaultValue;
  std::string currentValue;
  bool isDefault = true;
};

void setCommandLineUsageMessage(std::string_view usage);
std::vector<char*> parseCommandLine(int argc, char* argv[], std::string_view defaultFlagfilePath = {});
void logIgnoredCommandLineFlags();

[[nodiscard]] bool getCommandLineFlagInfo(std::string_view name, ZCommandLineFlagInfo* info);
[[nodiscard]] ZCommandLineFlagInfo getCommandLineFlagInfoOrDie(std::string_view name);
[[nodiscard]] bool commandLineFlagExists(std::string_view name);

[[nodiscard]] bool getCommandLineOption(std::string_view name, std::string* value);
[[nodiscard]] bool setCommandLineOption(std::string_view name, std::string_view value, std::string* error = nullptr);
[[nodiscard]] std::string commandLineFlagsToString();

} // namespace nim
