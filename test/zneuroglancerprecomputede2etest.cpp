#include "zneuroglancerprecomputed.h"

#include <QCoreApplication>

#include <folly/coro/BlockingWait.h>
#include "zcommandlineflags.h"
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>

ABSL_DECLARE_FLAG(uint64_t, atlas_disk_cache_http_max_bytes);

namespace nim {
namespace {

bool ciEnvironmentDetected()
{
  return std::getenv("CI") != nullptr || std::getenv("GITHUB_ACTIONS") != nullptr;
}

class ScopedQtCoreApplication
{
public:
  ScopedQtCoreApplication()
  {
    if (QCoreApplication::instance() != nullptr) {
      return;
    }

    static int argc = 1;
    static char arg0[] = "zneuroglancerprecomputede2etest";
    static char* argv[] = {arg0, nullptr};
    m_app = std::make_unique<QCoreApplication>(argc, argv);
  }

private:
  std::unique_ptr<QCoreApplication> m_app;
};

void runPublicDatasetSmokeTest(const char* backend)
{
  if (ciEnvironmentDetected()) {
    GTEST_SKIP() << "Network E2E tests are disabled on CI.";
  }

  ScopedQtCoreApplication qtApp;
  absl::FlagSaver flagSaver;
  ASSERT_TRUE(setCommandLineOption("atlas_http_backend", backend));
  absl::SetFlag(&FLAGS_atlas_disk_cache_http_max_bytes, 0);

  using namespace std::chrono_literals;
  constexpr auto timeout = 30s;

  struct Case
  {
    const char* name;
    const char* url;
  };

  const Case cases[] = {
    {"flyem_fib-25 image (jpeg)", "precomputed://gs://neuroglancer-public-data/flyem_fib-25/image"},
    {"flyem_fib-25 ground_truth (compressed_segmentation)", "precomputed://gs://neuroglancer-public-data/flyem_fib-25/ground_truth"},
  };

  for (const auto& c : cases) {
    SCOPED_TRACE(backend);
    SCOPED_TRACE(c.name);

    auto vol = ZNeuroglancerPrecomputedVolume::open(QString::fromUtf8(c.url), timeout);
    ASSERT_TRUE(vol);

    const auto chunks = vol->chunksIntersectingBaseBox(/*scaleIndex=*/0,
                                                       /*baseStart=*/{0, 0, 0},
                                                       /*baseEnd=*/{1, 1, 1});
    ASSERT_FALSE(chunks.empty());

    const auto img = folly::coro::blockingWait(vol->readChunkAsync(chunks.front()));
    ASSERT_TRUE(img);
    EXPECT_GT(img->byteNumber(), 0U);
  }
}

} // namespace

TEST(ZNeuroglancerPrecomputed, E2ESmokePublicDatasets)
{
  runPublicDatasetSmokeTest("proxygen");
}

TEST(ZNeuroglancerPrecomputed, E2ESmokePublicDatasetsCurl)
{
  runPublicDatasetSmokeTest("curl");
}

} // namespace nim
