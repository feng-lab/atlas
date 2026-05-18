#include "zglogbridge.h"

#include <memory>

#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif

#include <glog/flags.h>
#include <glog/logging.h>

#ifdef LOG
#undef LOG
#endif
#ifdef CHECK
#undef CHECK
#endif

#include <absl/base/log_severity.h>
#include <absl/log/check.h>
#include <absl/log/log.h>
#include <absl/strings/string_view.h>

namespace nim {
namespace {

constexpr const char* kGlogBridgeProgramName = "atlas-glog-bridge";

absl::LogSeverity toAbslSeverity(google::LogSeverity severity)
{
  switch (severity) {
    case google::GLOG_INFO:
      return absl::LogSeverity::kInfo;
    case google::GLOG_WARNING:
      return absl::LogSeverity::kWarning;
    case google::GLOG_ERROR:
      return absl::LogSeverity::kError;
    case google::GLOG_FATAL:
      return absl::LogSeverity::kFatal;
    default:
      return absl::LogSeverity::kInfo;
  }
}

const char* logLocationFile(const char* fullFilename, const char* baseFilename)
{
  if (fullFilename != nullptr && fullFilename[0] != '\0') {
    return fullFilename;
  }
  if (baseFilename != nullptr && baseFilename[0] != '\0') {
    return baseFilename;
  }
  return "glog";
}

void configureGlogForBridge()
{
  FLAGS_logtostderr = false;
  FLAGS_logtostdout = false;
  FLAGS_alsologtostderr = false;
  FLAGS_stderrthreshold = google::NUM_SEVERITIES;

  for (int severity = google::GLOG_INFO; severity < google::NUM_SEVERITIES; ++severity) {
    google::SetLogDestination(static_cast<google::LogSeverity>(severity), "");
  }
}

class GlogToAbseilLogBridge : public google::LogSink
{
public:
  GlogToAbseilLogBridge()
  {
    if (!google::IsGoogleLoggingInitialized()) {
      google::InitGoogleLogging(kGlogBridgeProgramName);
    }
    configureGlogForBridge();

    google::AddLogSink(this);
  }

  ~GlogToAbseilLogBridge() override
  {
    google::RemoveLogSink(this);
  }

  void send(google::LogSeverity severity,
            const char* fullFilename,
            const char* baseFilename,
            int line,
            const google::LogMessageTime& /*time*/,
            const char* message,
            size_t messageLen) override
  {
    const char* messageText = message != nullptr ? message : "";
    const absl::string_view messageView(messageText, message != nullptr ? messageLen : 0);
    const char* file = logLocationFile(fullFilename, baseFilename);
    if (severity == google::GLOG_FATAL) {
      LOG(FATAL).AtLocation(file, line) << "[glog FATAL] " << messageView;
      return;
    }

    const absl::LogSeverity abslSeverity = toAbslSeverity(severity);
    LOG(LEVEL(abslSeverity)).AtLocation(file, line) << messageView;
  }
};

std::unique_ptr<GlogToAbseilLogBridge>& glogBridge()
{
  static std::unique_ptr<GlogToAbseilLogBridge> bridge;
  return bridge;
}

} // namespace

void installGlogToAbseilLogBridge()
{
  auto& bridge = glogBridge();
  CHECK(!bridge) << "glog-to-Abseil log bridge must be installed exactly once";
  bridge = std::make_unique<GlogToAbseilLogBridge>();
}

void uninstallGlogToAbseilLogBridge()
{
  glogBridge().reset();
}

} // namespace nim
