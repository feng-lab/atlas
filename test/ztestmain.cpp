#include "zcommandlineflags.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

bool atlasTestStartupDiagnosticsEnabled()
{
  const char* value = std::getenv("ATLAS_TEST_STARTUP_DIAGNOSTICS");
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

void logAtlasTestPhase(const char* phase, const char* executable = nullptr)
{
  if (!atlasTestStartupDiagnosticsEnabled()) {
    return;
  }

  std::fputs("ATLAS_TEST_PHASE ", stderr);
  std::fputs(phase, stderr);
  if (executable != nullptr) {
    std::fputs(" exe=", stderr);
    std::fputs(executable, stderr);
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

void logAtlasTestEvent(const char* phase, const ::testing::TestInfo& testInfo)
{
  if (!atlasTestStartupDiagnosticsEnabled()) {
    return;
  }

  std::fputs("ATLAS_TEST_PHASE ", stderr);
  std::fputs(phase, stderr);
  std::fputs(" suite=", stderr);
  std::fputs(testInfo.test_suite_name(), stderr);
  std::fputs(" name=", stderr);
  std::fputs(testInfo.name(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

class AtlasTestEventDiagnostics final : public ::testing::EmptyTestEventListener
{
public:
  void OnTestStart(const ::testing::TestInfo& testInfo) override
  {
    logAtlasTestEvent("test-start", testInfo);
  }

  void OnTestEnd(const ::testing::TestInfo& testInfo) override
  {
    logAtlasTestEvent("test-end", testInfo);
  }
};

struct AtlasTestStartupDiagnostics
{
  AtlasTestStartupDiagnostics()
  {
    logAtlasTestPhase("static-init-ztestmain");
  }

  ~AtlasTestStartupDiagnostics()
  {
    logAtlasTestPhase("static-destroy-ztestmain");
  }
};

AtlasTestStartupDiagnostics g_atlasTestStartupDiagnostics;

} // namespace

int main(int argc, char** argv)
{
  logAtlasTestPhase("main-enter", argc > 0 ? argv[0] : nullptr);
  ::testing::InitGoogleTest(&argc, argv);
  logAtlasTestPhase("after-init-gtest", argc > 0 ? argv[0] : nullptr);
  nim::parseCommandLine(argc, argv);
  logAtlasTestPhase("after-parse-command-line", argc > 0 ? argv[0] : nullptr);
  if (atlasTestStartupDiagnosticsEnabled()) {
    ::testing::UnitTest::GetInstance()->listeners().Append(new AtlasTestEventDiagnostics);
  }
  const int result = RUN_ALL_TESTS();
  logAtlasTestPhase("after-run-all-tests", argc > 0 ? argv[0] : nullptr);
  return result;
}
