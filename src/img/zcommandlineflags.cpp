#include "zcommandlineflags.h"

#include <absl/flags/commandlineflag.h>
#include <absl/log/check.h>
#include <absl/log/log.h>
#include <algorithm>
#include <optional>
#include <sstream>
#include <vector>

namespace {

struct IgnoredCommandLineFlag
{
  absl::UnrecognizedFlag::Source source;
  std::string name;
};

std::vector<IgnoredCommandLineFlag>& ignoredCommandLineFlags()
{
  static std::vector<IgnoredCommandLineFlag> flags;
  return flags;
}

std::string flagTypeName(const absl::CommandLineFlag& flag)
{
  if (flag.IsOfType<bool>()) {
    return "bool";
  }
  if (flag.IsOfType<int>()) {
    return "int";
  }
  if (flag.IsOfType<int32_t>()) {
    return "int32";
  }
  if (flag.IsOfType<uint32_t>()) {
    return "uint32";
  }
  if (flag.IsOfType<int64_t>()) {
    return "int64";
  }
  if (flag.IsOfType<uint64_t>()) {
    return "uint64";
  }
  if (flag.IsOfType<double>()) {
    return "double";
  }
  if (flag.IsOfType<std::string>()) {
    return "string";
  }
  if (flag.IsOfType<std::optional<std::string>>()) {
    return "optional_string";
  }
  if (flag.IsOfType<std::vector<std::string>>()) {
    return "string_list";
  }
  return "unknown";
}

std::vector<char*> parseKnownCommandLineFlags(int argc, char* argv[])
{
  std::vector<char*> positionalArgs;
  std::vector<absl::UnrecognizedFlag> unrecognizedFlags;
  // User flagfiles can contain stale or platform-specific flags; keep valid flags and ignore the rest.
  absl::ParseAbseilFlagsOnly(argc, argv, positionalArgs, unrecognizedFlags);
  auto& ignoredFlags = ignoredCommandLineFlags();
  ignoredFlags.clear();
  ignoredFlags.reserve(unrecognizedFlags.size());
  for (const absl::UnrecognizedFlag& flag : unrecognizedFlags) {
    ignoredFlags.push_back({flag.source, flag.flag_name});
  }
  return positionalArgs;
}

} // namespace

namespace nim {

void setCommandLineUsageMessage(std::string_view usage)
{
  absl::SetProgramUsageMessage(usage);
}

std::vector<char*> parseCommandLine(int argc, char* argv[], std::string_view defaultFlagfilePath)
{
  if (defaultFlagfilePath.empty()) {
    return parseKnownCommandLineFlags(argc, argv);
  }

  std::string defaultFlagfileArg = "--flagfile=" + std::string(defaultFlagfilePath);
  std::vector<char*> parseArgs;
  parseArgs.reserve(static_cast<size_t>(argc) + 1);
  parseArgs.push_back(argv[0]);
  parseArgs.push_back(defaultFlagfileArg.data());
  for (int i = 1; i < argc; ++i) {
    parseArgs.push_back(argv[i]);
  }
  return parseKnownCommandLineFlags(static_cast<int>(parseArgs.size()), parseArgs.data());
}

void logIgnoredCommandLineFlags()
{
  for (const IgnoredCommandLineFlag& flag : ignoredCommandLineFlags()) {
    const char* source = flag.source == absl::UnrecognizedFlag::kFromFlagfile ? "flagfile" : "argv";
    LOG(INFO) << "ignored unknown command line flag from " << source << ": --" << flag.name;
  }
}

bool getCommandLineFlagInfo(std::string_view name, ZCommandLineFlagInfo* info)
{
  CHECK(info != nullptr);
  absl::CommandLineFlag* flag = absl::FindCommandLineFlag(name);
  if (flag == nullptr || flag->IsRetired()) {
    return false;
  }
  info->name = std::string(flag->Name());
  info->type = flagTypeName(*flag);
  info->description = flag->Help();
  info->defaultValue = flag->DefaultValue();
  info->currentValue = flag->CurrentValue();
  info->isDefault = info->currentValue == info->defaultValue;
  return true;
}

ZCommandLineFlagInfo getCommandLineFlagInfoOrDie(std::string_view name)
{
  ZCommandLineFlagInfo info;
  CHECK(getCommandLineFlagInfo(name, &info)) << "Unknown command line flag: " << name;
  return info;
}

bool commandLineFlagExists(std::string_view name)
{
  absl::CommandLineFlag* flag = absl::FindCommandLineFlag(name);
  return flag != nullptr && !flag->IsRetired();
}

bool getCommandLineOption(std::string_view name, std::string* value)
{
  CHECK(value != nullptr);
  absl::CommandLineFlag* flag = absl::FindCommandLineFlag(name);
  if (flag == nullptr || flag->IsRetired()) {
    return false;
  }
  *value = flag->CurrentValue();
  return true;
}

bool setCommandLineOption(std::string_view name, std::string_view value, std::string* error)
{
  absl::CommandLineFlag* flag = absl::FindCommandLineFlag(name);
  if (flag == nullptr || flag->IsRetired()) {
    if (error != nullptr) {
      *error = "unknown flag";
    }
    return false;
  }

  std::string parseError;
  if (!flag->ParseFrom(value, &parseError)) {
    if (error != nullptr) {
      *error = parseError;
    }
    return false;
  }
  return true;
}

std::string commandLineFlagsToString()
{
  auto flags = absl::GetAllFlags();
  std::vector<absl::CommandLineFlag*> orderedFlags;
  orderedFlags.reserve(flags.size());
  for (const auto& [name, flag] : flags) {
    if (flag != nullptr && !flag->IsRetired()) {
      orderedFlags.push_back(flag);
    }
  }

  std::sort(orderedFlags.begin(),
            orderedFlags.end(),
            [](const absl::CommandLineFlag* a, const absl::CommandLineFlag* b) {
              return a->Name() < b->Name();
            });

  std::ostringstream out;
  for (const absl::CommandLineFlag* flag : orderedFlags) {
    out << "--" << flag->Name() << "=" << flag->CurrentValue() << '\n';
  }
  return out.str();
}

} // namespace nim
