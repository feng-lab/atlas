#include "zcommandlineflags.h"
#include "zimginit.h"
#include "ztest.h"
#include <gtest/gtest.h>

#include <QString>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

void printStartupPhase(const char* phase)
{
  if (std::getenv("ATLAS_TEST_STARTUP_DIAGNOSTICS") == nullptr) {
    return;
  }
#ifdef _WIN32
  const int pid = _getpid();
#else
  const int pid = getpid();
#endif
  std::fprintf(stderr, "ATLAS_TEST_STARTUP pid=%d phase=%s\n", pid, phase);
  std::fflush(stderr);
}

class AtlasImgRuntimeEnvironment final : public ::testing::Environment
{
public:
  void SetUp() override
  {
    printStartupPhase("gtest_environment.before_zimginit");
    (void)nim::ZImgInit::instance(QString(),
                                  nim::atlasTestJreDir().absolutePath(),
                                  nim::atlasTestJarsDir().absolutePath(),
                                  false);
    printStartupPhase("gtest_environment.after_zimginit");
  }

  void TearDown() override
  {
    printStartupPhase("gtest_environment.before_zimg_shutdown");
    nim::ZImgInit::shutdown();
    printStartupPhase("gtest_environment.after_zimg_shutdown");
  }
};

} // namespace

int main(int argc, char** argv)
{
  printStartupPhase("main.before_init_google_test");
  ::testing::InitGoogleTest(&argc, argv);
  printStartupPhase("main.after_init_google_test");
  nim::parseCommandLine(argc, argv);
  printStartupPhase("main.after_parse_command_line");
  ::testing::AddGlobalTestEnvironment(new AtlasImgRuntimeEnvironment);
  printStartupPhase("main.before_run_all_tests");
  return RUN_ALL_TESTS();
}
