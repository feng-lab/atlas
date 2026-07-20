#include "z3dperfcollector.h"

#include "zcommandlineflags.h"
#include "zjson.h"

#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <string>

namespace nim {
namespace {

TEST(Z3DPerfCollectorTest, JsonSummaryCarriesVersionedSubmissionMetrics)
{
  absl::FlagSaver flagSaver;
  QTemporaryDir temporaryDirectory;
  ASSERT_TRUE(temporaryDirectory.isValid());
  const std::string summaryPath = temporaryDirectory.filePath("perf.ndjson").toStdString();
  std::string flagError;
  ASSERT_TRUE(setCommandLineOption("atlas_perf_summary", "json:" + summaryPath, &flagError)) << flagError;

  constexpr uint64_t token = 0xf000000000000001u;
  constexpr uint32_t submissionId = 1u;
  Z3DPerfCollector::Stats stats{};
  stats.fenceWaits = 2u;
  auto& collector = Z3DPerfCollector::instance();
  collector.noteSubmissionStarted(token, submissionId);
  collector.addSubmission(token, submissionId, 1.25, {}, {}, stats);
  collector.markClosed(token);

  std::ifstream stream(summaryPath);
  ASSERT_TRUE(stream.is_open());
  std::string line;
  ASSERT_TRUE(std::getline(stream, line));
  EXPECT_FALSE(line.empty());
  std::string extraLine;
  EXPECT_FALSE(std::getline(stream, extraLine));

  const json::value parsed = json::parse(line);
  const json::object& record = parsed.as_object();
  constexpr std::array<const char*, 8> expectedRootKeys{
    "schema",
    "schema_version",
    "frame",
    "cpu_ms",
    "gpu_ms",
    "gpu_scoped_ms",
    "top",
    "stats",
  };
  EXPECT_EQ(record.size(), expectedRootKeys.size());
  for (const char* key : expectedRootKeys) {
    EXPECT_NE(record.if_contains(key), nullptr) << "Missing version-1 performance root field: " << key;
  }
  EXPECT_EQ(record.at("schema").as_string(), "atlas.perf.frame");
  EXPECT_EQ(json::value_to<uint64_t>(record.at("schema_version")), 1u);
  const json::object& serializedStats = record.at("stats").as_object();
  constexpr std::array<const char*, 21> expectedStatsKeys{
    "upload_hi",
    "static_staged",
    "lines_staged",
    "fonts_staged",
    "meshes_staged",
    "spheres_staged",
    "cones_staged",
    "ellipsoids_staged",
    "readback",
    "submissions",
    "fence_waits",
    "all_ms",
    "all_samples",
    "descriptor_sets",
    "pipelines_created",
    "pipelines_bound",
    "segments",
    "clears",
    "loads",
    "descriptor_writes_recording",
    "bound_set_rewrites",
  };
  EXPECT_EQ(serializedStats.size(), expectedStatsKeys.size());
  for (const char* key : expectedStatsKeys) {
    EXPECT_NE(serializedStats.if_contains(key), nullptr) << "Missing version-1 performance statistic: " << key;
  }
  EXPECT_EQ(json::value_to<uint64_t>(serializedStats.at("submissions")), 1u);
  EXPECT_EQ(json::value_to<uint64_t>(serializedStats.at("fence_waits")), 2u);
}

} // namespace
} // namespace nim
