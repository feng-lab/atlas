#include "zrunneutucommand2.h"

#include <QDir>
#include <QFileInfo>

#include "zneutubelegacy.h"
#include "zneutubeskeletonize.h"

#include "zlog.h"

#include "zjson.h"

#include <gflags/gflags.h>

#include <charconv>
#include <optional>
#include <string>
#include <vector>

DECLARE_int32(v);

namespace nim {

namespace {

struct Command2Args
{
  std::vector<std::string> input;
  std::string output;

  // Default Resources/json directory injected by the host app (Atlas).
  // Used only as a fallback when caller does not explicitly provide --config.
  std::string jsonDirPath;

  // Path to command_config.json (or equivalent). Controls default locations for skeletonize/trace configs.
  std::string commandConfigPath;

  std::string generalConfig; // JSON string or JSON file path for --general

  std::optional<std::array<int, 3>> downsampleInterval;
  std::optional<std::array<int, 3>> position;
  std::optional<std::array<int, 3>> size;

  int level = 0;
  double scale = 1.0;

  bool isVerbose = false;
  bool diagnosis = false;

  bool runSkeletonize = false;
  bool runTraceNeuron = false;
  bool runCompareSwc = false;
  bool runComputeSeed = false;
  bool runGeneral = false;
};

[[nodiscard]] bool fileExists(const QString& path)
{
  const QFileInfo fi(path);
  return fi.exists() && fi.isFile();
}

[[nodiscard]] QString resolvePathRelativeTo(const QString& baseFilePath, const QString& maybeRelativePath)
{
  const QFileInfo fileInfo(maybeRelativePath);
  if (fileInfo.isRelative()) {
    return QDir(QFileInfo(baseFilePath).absolutePath()).absoluteFilePath(fileInfo.filePath());
  }
  return fileInfo.absoluteFilePath();
}

[[nodiscard]] std::optional<std::string>
extractIncludePath(const json::object& root, const QString& baseConfigFilePath, const char* key)
{
  auto it = root.find(key);
  if (it == root.end() || !it->value().is_object()) {
    return std::nullopt;
  }

  const json::object& sub = it->value().as_object();
  auto includeIt = sub.find("include");
  if (includeIt == sub.end() || !includeIt->value().is_string()) {
    return std::nullopt;
  }

  const auto includeText = QString::fromStdString(std::string(includeIt->value().as_string().c_str()));
  const QString resolved = resolvePathRelativeTo(baseConfigFilePath, includeText);
  return resolved.toStdString();
}

[[nodiscard]] json::value parseJsonValue(std::string_view text, std::string_view context)
{
  json::parse_options opt; // all extensions default to off
  opt.allow_comments = true;
  opt.allow_trailing_commas = true;
  opt.allow_infinity_and_nan = true;

  try {
    return json::parse(text, json::storage_ptr(), opt);
  }
  catch (const std::exception& e) {
    throw std::runtime_error(fmt::format("Failed to parse JSON ({}): {}", context, e.what()));
  }
}

[[nodiscard]] json::object loadJsonObjectOrThrow(const QString& filePath)
{
  try {
    return nim::loadJsonObject(filePath);
  }
  catch (const std::exception& e) {
    throw std::runtime_error(fmt::format("Failed to load JSON file '{}': {}", filePath.toStdString(), e.what()));
  }
}

[[nodiscard]] json::object loadJsonObjectFromTextOrFileOrThrow(const std::string& textOrPath, std::string_view context)
{
  const QString q = QString::fromStdString(textOrPath);
  const QFileInfo fi(q);
  if (fi.exists() && fi.isFile() && fi.suffix().compare("json", Qt::CaseInsensitive) == 0) {
    return loadJsonObjectOrThrow(fi.absoluteFilePath());
  }

  json::value v = parseJsonValue(textOrPath, context);
  if (!v.is_object()) {
    throw std::runtime_error(fmt::format("Expected JSON object for {}, got {}", context, jsonTypeName(v)));
  }
  return std::move(v.as_object());
}

[[nodiscard]] std::optional<int> parseInt(std::string_view s)
{
  int out = 0;
  const auto* begin = s.data();
  const auto* end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(begin, end, out);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return out;
}

[[nodiscard]] std::optional<double> parseDouble(std::string_view s)
{
  try {
    size_t idx = 0;
    double v = std::stod(std::string(s), &idx);
    if (idx != s.size()) {
      return std::nullopt;
    }
    return v;
  }
  catch (...) {
    return std::nullopt;
  }
}

[[nodiscard]] bool parseArray3Int(const json::value& v, std::array<int, 3>* out)
{
  CHECK(out != nullptr);
  if (!v.is_array()) {
    return false;
  }
  const auto& arr = v.as_array();
  if (arr.size() != 3) {
    return false;
  }
  for (size_t i = 0; i < 3; ++i) {
    if (!arr[i].is_int64()) {
      return false;
    }
    (*out)[i] = static_cast<int>(arr[i].as_int64());
  }
  return true;
}

[[nodiscard]] std::optional<std::string> defaultCommandConfigPathFromJsonDir(std::string_view jsonDirPath)
{
  if (jsonDirPath.empty()) {
    return std::nullopt;
  }
  const QDir jsonDir(QString::fromUtf8(jsonDirPath.data(), static_cast<qsizetype>(jsonDirPath.size())));
  return jsonDir.absoluteFilePath("command_config.json").toStdString();
}

[[nodiscard]] bool parseArgs(int argc, char* argv[], std::string_view injectedJsonDirPath, Command2Args* out)
{
  CHECK(out != nullptr);
  CHECK(argc >= 2);
  CHECK(argv != nullptr);
  CHECK(argv[1] != nullptr);

  // argv[1] must be "--command2" (Atlas main routes here only in that case).
  if (std::string_view(argv[1]) != "--command2") {
    LOG(ERROR) << "ZRunNeuTuCommand2 must be invoked via '--command2' as argv[1].";
    return false;
  }

  out->jsonDirPath = std::string(injectedJsonDirPath);

  bool hasExplicitCommandConfig = false;
  if (auto defaultPath = defaultCommandConfigPathFromJsonDir(out->jsonDirPath)) {
    out->commandConfigPath = *defaultPath;
  }

  for (int i = 2; i < argc; ++i) {
    CHECK(argv[i] != nullptr);
    const std::string_view arg(argv[i]);

    if (arg == "--verbose") {
      out->isVerbose = true;
      FLAGS_v = 1;
      continue;
    }

    if (arg == "-o") {
      if (i + 1 >= argc) {
        LOG(ERROR) << "Missing value for -o";
        return false;
      }
      out->output = argv[++i];
      continue;
    }

    if (arg == "--config") {
      if (i + 1 >= argc) {
        LOG(ERROR) << "Missing value for --config";
        return false;
      }
      out->commandConfigPath = argv[++i];
      hasExplicitCommandConfig = true;
      continue;
    }

    if (arg == "--json_dir") {
      if (i + 1 >= argc) {
        LOG(ERROR) << "Missing value for --json_dir";
        return false;
      }
      out->jsonDirPath = argv[++i];
      if (!hasExplicitCommandConfig) {
        if (auto defaultPath = defaultCommandConfigPathFromJsonDir(out->jsonDirPath)) {
          out->commandConfigPath = *defaultPath;
        } else {
          out->commandConfigPath.clear();
        }
      }
      continue;
    }

    if (arg == "--intv") {
      if (i + 3 >= argc) {
        LOG(ERROR) << "Missing values for --intv <x> <y> <z>";
        return false;
      }
      std::array<int, 3> intv{};
      for (int k = 0; k < 3; ++k) {
        const std::string_view v(argv[i + 1 + k]);
        const auto parsed = parseInt(v);
        if (!parsed) {
          LOG(ERROR) << "Invalid --intv value: " << std::string(v);
          return false;
        }
        intv[static_cast<size_t>(k)] = *parsed;
      }
      out->downsampleInterval = intv;
      i += 3;
      continue;
    }

    if (arg == "--scale") {
      if (i + 1 >= argc) {
        LOG(ERROR) << "Missing value for --scale";
        return false;
      }
      const std::string_view v(argv[++i]);
      const auto parsed = parseDouble(v);
      if (!parsed) {
        LOG(ERROR) << "Invalid --scale value: " << std::string(v);
        return false;
      }
      out->scale = *parsed;
      continue;
    }

    if (arg == "--level") {
      if (i + 1 >= argc) {
        LOG(ERROR) << "Missing value for --level";
        return false;
      }
      const std::string_view v(argv[++i]);
      const auto parsed = parseInt(v);
      if (!parsed) {
        LOG(ERROR) << "Invalid --level value: " << std::string(v);
        return false;
      }
      out->level = *parsed;
      continue;
    }

    if (arg == "--skeletonize") {
      out->runSkeletonize = true;
      continue;
    }
    if (arg == "--trace") {
      out->runTraceNeuron = true;
      continue;
    }
    if (arg == "--compare_swc") {
      out->runCompareSwc = true;
      continue;
    }
    if (arg == "--compute_seed") {
      out->runComputeSeed = true;
      continue;
    }
    if (arg == "--general") {
      if (i + 1 >= argc) {
        LOG(ERROR) << "Missing value for --general";
        return false;
      }
      out->runGeneral = true;
      out->generalConfig = argv[++i];
      continue;
    }

    if (!arg.empty() && arg[0] == '-') {
      LOG(ERROR) << "Unknown flag: " << std::string(arg);
      return false;
    }

    // Positional input(s).
    out->input.emplace_back(arg);
  }

  return true;
}

[[nodiscard]] json::object loadCommandConfigOrThrow(const std::string& path)
{
  const QString q = QString::fromStdString(path);
  if (!fileExists(q)) {
    throw std::runtime_error(fmt::format("Missing command config file '{}'", path));
  }
  return loadJsonObjectOrThrow(QFileInfo(q).absoluteFilePath());
}

} // namespace

int ZRunNeuTuCommand2::run(int argc, char* argv[])
{
  return run(argc, argv, std::string_view{});
}

int ZRunNeuTuCommand2::run(int argc, char* argv[], std::string_view jsonDirPath)
{
  Command2Args args;
  if (!parseArgs(argc, argv, jsonDirPath, &args)) {
    LOG(INFO) << "Usage (v2): Atlas --command2 [<input> ...] [-o <output>] [--config <command_config.json>]"
                 " [--json_dir <Resources/json>]"
                 " [--intv x y z] [--skeletonize] [--general <json|path>] [--compare_swc --scale <s>]"
                 " [--trace --level <n>] [--verbose]";
    return 1;
  }

  if (args.commandConfigPath.empty()) {
    LOG(ERROR) << "Missing command config: provide --config <command_config.json> or set a JSON dir (--json_dir) "
                  "or invoke via Atlas (injects Resources/json).";
    return 1;
  }

  for (int i = 0; i < argc; ++i) {
    LOG(INFO) << argv[i];
  }

  // Command selection: preserve legacy precedence if multiple flags are specified.
  enum class Command
  {
    Skeletonize,
    Trace,
    CompareSwc,
    ComputeSeed,
    General,
    Unknown
  };

  Command command = Command::Unknown;
  if (args.runSkeletonize) {
    command = Command::Skeletonize;
  } else if (args.runTraceNeuron) {
    command = Command::Trace;
  } else if (args.runCompareSwc) {
    command = Command::CompareSwc;
  } else if (args.runComputeSeed) {
    command = Command::ComputeSeed;
  } else if (args.runGeneral) {
    command = Command::General;
  }

  // Load command config (used to resolve include files, preserving the existing layout under Resources/json).
  json::object commandConfig;
  try {
    commandConfig = loadCommandConfigOrThrow(args.commandConfigPath);
  }
  catch (const std::exception& e) {
    LOG(ERROR) << e.what();
    return 1;
  }

  const QString commandConfigPathQ = QString::fromStdString(args.commandConfigPath);
  const QString commandConfigAbsPathQ = QFileInfo(commandConfigPathQ).absoluteFilePath();

  const auto skeletonizeInclude =
    extractIncludePath(commandConfig, commandConfigAbsPathQ, "skeletonize").value_or(std::string{});
  const auto traceInclude = extractIncludePath(commandConfig, commandConfigAbsPathQ, "trace").value_or(std::string{});

  // Parse optional "json input" mode: `input[0] == "json"` and `input[1]` is JSON string or JSON file path.
  json::object inputJson;
  if (!args.input.empty() && args.input[0] == "json") {
    if (args.input.size() < 2) {
      LOG(ERROR) << "Invalid input: expected `json <jsonString|jsonFile>`";
      return 1;
    }

    try {
      inputJson = loadJsonObjectFromTextOrFileOrThrow(args.input[1], "input json");
    }
    catch (const std::exception& e) {
      LOG(ERROR) << e.what();
      return 1;
    }

    if (auto it = inputJson.find("position"); it != inputJson.end()) {
      std::array<int, 3> pos{};
      if (!parseArray3Int(it->value(), &pos)) {
        LOG(ERROR) << "Invalid input.position: expected array[3] of int";
        return 1;
      }
      args.position = pos;
    }

    if (auto it = inputJson.find("size"); it != inputJson.end()) {
      std::array<int, 3> sz{};
      if (!parseArray3Int(it->value(), &sz)) {
        LOG(ERROR) << "Invalid input.size: expected array[3] of int";
        return 1;
      }
      args.size = sz;
    }

    // Rewrite positional inputs to match legacy expectations:
    //  - input[0] becomes "signal"
    //  - input[1] becomes "swc" (optional)
    // Keep any extra positional arguments intact to avoid truncation.
    CHECK(args.input.size() >= 2);

    if (auto it = inputJson.find("swc"); it != inputJson.end() && it->value().is_string()) {
      args.input[1] = std::string(it->value().as_string().c_str());
    } else {
      args.input[1].clear();
    }

    args.input[0].clear();
    if (auto it = inputJson.find("signal"); it != inputJson.end() && it->value().is_string()) {
      args.input[0] = std::string(it->value().as_string().c_str());
    }
  }

  // Dispatch.
  switch (command) {
    case Command::Skeletonize: {
      if (args.input.empty() || args.input[0].empty()) {
        LOG(ERROR) << "Skeletonize: missing input file.";
        return 1;
      }
      return neutube::runSkeletonize(args.input[0],
                                     args.output,
                                     skeletonizeInclude,
                                     args.downsampleInterval,
                                     args.isVerbose);
    }

    case Command::CompareSwc: {
      return neutube_legacy::runCompareSwc(args.input, args.scale);
    }

    case Command::Trace: {
      return neutube_legacy::runTrace(args.input,
                                      args.output,
                                      args.position,
                                      args.level,
                                      args.diagnosis,
                                      traceInclude,
                                      args.jsonDirPath,
                                      args.isVerbose);
    }

    case Command::General: {
      if (args.generalConfig.empty()) {
        LOG(ERROR) << "General: missing --general config (JSON string or JSON file path).";
        return 1;
      }

      json::object generalCfg;
      try {
        generalCfg = loadJsonObjectFromTextOrFileOrThrow(args.generalConfig, "--general config");
      }
      catch (const std::exception& e) {
        LOG(ERROR) << e.what();
        return 1;
      }
      return neutube_legacy::runGeneral(args.generalConfig,
                                        generalCfg,
                                        inputJson,
                                        args.input,
                                        args.output,
                                        args.level,
                                        args.diagnosis,
                                        traceInclude,
                                        args.jsonDirPath,
                                        args.isVerbose);
    }

    case Command::ComputeSeed:
    case Command::Unknown:
      LOG(INFO) << "Unknown command";
      return 1;
  }
}

} // namespace nim
