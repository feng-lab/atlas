#include "zcommandlineflags.h"
#include "zhttptruststore.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace nim {
namespace {

[[maybe_unused]] auto* const kResolveHttpTrustStoreConfigForLink = &resolveHttpTrustStoreConfig;

TEST(ZHttpTrustStore, ParsesWindowsTrustSourceValues)
{
  absl::FlagSaver flagSaver;

  for (const auto& [inputValue, canonicalValue] : {
         std::pair{"auto",          "auto"         },
         std::pair{"Windows_Store", "windows_store"},
         std::pair{"BUNDLED_PEM",   "bundled_pem"  }
  }) {
    std::string error;
    EXPECT_TRUE(setCommandLineOption("atlas_http_windows_trust_source", inputValue, &error)) << error;

    std::string storedValue;
    ASSERT_TRUE(getCommandLineOption("atlas_http_windows_trust_source", &storedValue));
    EXPECT_EQ(storedValue, canonicalValue);
  }
}

TEST(ZHttpTrustStore, RejectsInvalidWindowsTrustSource)
{
  absl::FlagSaver flagSaver;

  for (const std::string value : {"bad_value", "systemstore", "bundle"}) {
    std::string error;
    EXPECT_FALSE(setCommandLineOption("atlas_http_windows_trust_source", value, &error));
    EXPECT_FALSE(error.empty());
  }
}

} // namespace
} // namespace nim
