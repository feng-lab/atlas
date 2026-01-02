#include "zneuroglancerprecomputed.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <string>

namespace nim {
namespace {

bool envFlagEnabled(const char* name)
{
  const char* v = std::getenv(name);
  if (!v) {
    return false;
  }
  const std::string s(v);
  return s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "YES";
}

} // namespace

TEST(ZNeuroglancerPrecomputed, E2ESmokePublicDatasets)
{
  if (!envFlagEnabled("ATLAS_ENABLE_NETWORK_TESTS")) {
    GTEST_SKIP() << "Set ATLAS_ENABLE_NETWORK_TESTS=1 to run network E2E tests.";
  }

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
    SCOPED_TRACE(c.name);

    auto vol = ZNeuroglancerPrecomputedVolume::open(QString::fromUtf8(c.url), timeout);
    ASSERT_TRUE(vol);

    const auto chunks = vol->chunksIntersectingBaseBox(/*scaleIndex=*/0,
                                                       /*baseStart=*/{0, 0, 0},
                                                       /*baseEnd=*/{1, 1, 1});
    ASSERT_FALSE(chunks.empty());

    const auto img = vol->readChunkBlocking(chunks.front());
    ASSERT_TRUE(img);
    EXPECT_GT(img->byteNumber(), 0U);
  }
}

} // namespace nim

