#include <gtest/gtest.h>

#include "zblockedautotracesession.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

[[nodiscard]] QString commitDirName(uint64_t commitId)
{
  return QStringLiteral("commit_%1").arg(static_cast<qulonglong>(commitId), 6, 10, QLatin1Char('0'));
}

void writeEmptyFileOrThrow(const QString& path)
{
  QFile f(path);
  ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate)) << path.toStdString();
  f.close();
}

TEST(ZBlockedAutoTraceSession, WritesAndResumesSequentialCommits)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString sessionDir = QDir(tmp.path()).absoluteFilePath("session");
  nim::ZBlockedAutoTraceSession session(sessionDir);

  nim::ZBlockedAutoTraceManifest manifest;
  manifest.formatVersion = nim::kBlockedAutoTraceManifestFormatVersion;
  manifest.datasetId = "unit_test_dataset";
  manifest.channel = 0;
  manifest.time = 0;
  manifest.signalDownsampleRatio = {1, 1, 1};
  manifest.zScale = 2.5;
  manifest.datasetShape = {.width = 100, .height = 80, .depth = 60};
  manifest.block = {.coreX = 16, .coreY = 16, .coreZ = 16, .halo = 2};
  manifest.subtractConstant = 0.0;
  manifest.traceConfig = nim::TraceConfig{};

  ASSERT_NO_THROW(session.ensureCreatedOrThrow(manifest));

  {
    const nim::ZBlockedAutoTraceLoadedState state = session.loadLatestOrEmptyOrThrow();
    EXPECT_EQ(state.commitId, 0u);
    EXPECT_TRUE(state.swc.empty());
    EXPECT_TRUE(state.frontier.empty());
    EXPECT_EQ(state.scheduler.nextLinearBlockIndex, 0u);
  }

  nim::ZBlockedAutoTraceCommitWrite c1;
  c1.info.formatVersion = nim::kBlockedAutoTraceCommitFormatVersion;
  c1.info.commitId = 1;
  c1.info.blockId = {.bx = 0, .by = 0, .bz = 0};
  c1.info.didSeedScan = true;
  c1.scheduler.nextLinearBlockIndex = 7;

  c1.swcDeltaNodes.push_back(nim::ZBlockedAutoTraceSwcDeltaNode{
    .id = 2,
    .type = 0,
    .x = 1.0,
    .y = 2.0,
    .z = 3.0,
    .radius = 1.0,
    .parentId = 1,
  });
  c1.swcDeltaNodes.push_back(nim::ZBlockedAutoTraceSwcDeltaNode{
    .id = 1,
    .type = 0,
    .x = 0.0,
    .y = 0.0,
    .z = 0.0,
    .radius = 2.0,
    .parentId = -1,
  });

  ASSERT_NO_THROW(session.writeCommitOrThrow(c1));

  nim::ZBlockedAutoTraceCommitWrite c2;
  c2.info.formatVersion = nim::kBlockedAutoTraceCommitFormatVersion;
  c2.info.commitId = 2;
  c2.info.blockId = {.bx = 1, .by = 0, .bz = 0};
  c2.info.didSeedScan = false;
  c2.scheduler.nextLinearBlockIndex = 8;

  c2.swcDeltaNodes.push_back(nim::ZBlockedAutoTraceSwcDeltaNode{
    .id = 3,
    .type = 0,
    .x = 2.0,
    .y = 2.0,
    .z = 3.0,
    .radius = 1.0,
    .parentId = 2,
  });

  nim::ZBlockedAutoTracePendingTask task;
  task.taskId = 9;
  task.attachSwcNodeId = 3;
  task.direction = nim::TraceDirection::Forward;
  task.endLocseg.seg.r1 = 1.0;
  task.endLocseg.seg.c = 0.0;
  task.endLocseg.seg.h = 11.0;
  task.endLocseg.seg.theta = 0.0;
  task.endLocseg.seg.psi = 0.0;
  task.endLocseg.seg.curvature = 0.0;
  task.endLocseg.seg.alpha = 0.0;
  task.endLocseg.seg.scale = 1.0;
  task.endLocseg.pos = {2.0, 2.0, 3.0};
  task.reason = "UnitTest";
  task.suggestedBlock = {.bx = 1, .by = 0, .bz = 0};
  c2.frontier.push_back(task);

  ASSERT_NO_THROW(session.writeCommitOrThrow(c2));

  const nim::ZBlockedAutoTraceLoadedState state = session.loadLatestOrEmptyOrThrow();
  EXPECT_EQ(state.commitId, 2u);
  EXPECT_EQ(state.scheduler.nextLinearBlockIndex, 8u);
  ASSERT_EQ(state.seedScannedBlocks.size(), 1u);
  EXPECT_EQ(state.seedScannedBlocks[0].bx, 0);
  EXPECT_EQ(state.seedScannedBlocks[0].by, 0);
  EXPECT_EQ(state.seedScannedBlocks[0].bz, 0);

  EXPECT_EQ(state.swc.size(), 3u);
  ASSERT_TRUE(state.nodeById.contains(1));
  ASSERT_TRUE(state.nodeById.contains(2));
  ASSERT_TRUE(state.nodeById.contains(3));

  // Check parent linkage after replay.
  {
    auto it2 = state.nodeById.find(2);
    ASSERT_NE(it2, state.nodeById.end());
    EXPECT_EQ(nim::ZSwc::parentID(it2->second), 1);

    auto it3 = state.nodeById.find(3);
    ASSERT_NE(it3, state.nodeById.end());
    EXPECT_EQ(nim::ZSwc::parentID(it3->second), 2);
  }

  ASSERT_EQ(state.frontier.size(), 1u);
  EXPECT_EQ(state.frontier[0].taskId, 9u);
  EXPECT_EQ(state.frontier[0].attachSwcNodeId, 3);
  EXPECT_EQ(state.frontier[0].direction, nim::TraceDirection::Forward);
  EXPECT_EQ(state.frontier[0].reason, "UnitTest");
}

TEST(ZBlockedAutoTraceSession, RollingSwcCatchupFromCommits)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString sessionDir = QDir(tmp.path()).absoluteFilePath("session");
  nim::ZBlockedAutoTraceSession session(sessionDir);

  nim::ZBlockedAutoTraceManifest manifest;
  manifest.formatVersion = nim::kBlockedAutoTraceManifestFormatVersion;
  manifest.datasetId = "unit_test_dataset";
  manifest.channel = 0;
  manifest.time = 0;
  manifest.signalDownsampleRatio = {1, 1, 1};
  manifest.zScale = 2.5;
  manifest.datasetShape = {.width = 100, .height = 80, .depth = 60};
  manifest.block = {.coreX = 16, .coreY = 16, .coreZ = 16, .halo = 2};
  manifest.subtractConstant = 0.0;
  manifest.traceConfig = nim::TraceConfig{};

  ASSERT_NO_THROW(session.ensureCreatedOrThrow(manifest));

  nim::ZBlockedAutoTraceCommitWrite c1;
  c1.info.formatVersion = nim::kBlockedAutoTraceCommitFormatVersion;
  c1.info.commitId = 1;
  c1.info.blockId = {.bx = 0, .by = 0, .bz = 0};
  c1.info.didSeedScan = true;
  c1.scheduler.nextLinearBlockIndex = 1;
  c1.swcDeltaNodes.push_back(nim::ZBlockedAutoTraceSwcDeltaNode{
    .id = 2,
    .type = 0,
    .x = 1.0,
    .y = 2.0,
    .z = 3.0,
    .radius = 1.0,
    .parentId = 1,
  });
  c1.swcDeltaNodes.push_back(nim::ZBlockedAutoTraceSwcDeltaNode{
    .id = 1,
    .type = 0,
    .x = 0.0,
    .y = 0.0,
    .z = 0.0,
    .radius = 2.0,
    .parentId = -1,
  });
  ASSERT_NO_THROW(session.writeCommitOrThrow(c1));

  nim::ZBlockedAutoTraceCommitWrite c2;
  c2.info.formatVersion = nim::kBlockedAutoTraceCommitFormatVersion;
  c2.info.commitId = 2;
  c2.info.blockId = {.bx = 1, .by = 0, .bz = 0};
  c2.info.didSeedScan = true;
  c2.scheduler.nextLinearBlockIndex = 2;
  c2.swcDeltaNodes.push_back(nim::ZBlockedAutoTraceSwcDeltaNode{
    .id = 3,
    .type = 0,
    .x = 2.0,
    .y = 2.0,
    .z = 3.0,
    .radius = 1.0,
    .parentId = 2,
  });
  ASSERT_NO_THROW(session.writeCommitOrThrow(c2));

  // Simulate a tracer that only appends rolling SWC starting at commit 2; it must catch up commit 1 from disk.
  ASSERT_NO_THROW(session.appendToRollingSwcOrThrow(2, c2.swcDeltaNodes));

  const QString rollingPath = QDir(sessionDir).absoluteFilePath("result_tracing.swc");
  ASSERT_TRUE(QFileInfo::exists(rollingPath)) << rollingPath.toStdString();

  std::ifstream file(rollingPath.toStdString(), std::ios_base::in);
  ASSERT_TRUE(file.is_open());

  std::vector<int64_t> ids;
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream in(line);
    int64_t id = 0;
    in >> id;
    if (!in.fail()) {
      ids.push_back(id);
    }
  }

  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], 2);
  EXPECT_EQ(ids[1], 1);
  EXPECT_EQ(ids[2], 3);

  const QString statePath = QDir(sessionDir).absoluteFilePath("result_tracing_state.json");
  ASSERT_TRUE(QFileInfo::exists(statePath)) << statePath.toStdString();
  const json::object st = nim::loadJsonObject(statePath);
  EXPECT_EQ(json::value_to<int>(st.at("format_version")), 1);
  EXPECT_EQ(json::value_to<uint64_t>(st.at("commit_id")), 2u);
  EXPECT_EQ(json::value_to<int64_t>(st.at("byte_size")), static_cast<int64_t>(QFileInfo(rollingPath).size()));
}

TEST(ZBlockedAutoTraceSession, IgnoresIncompleteAndNonContiguousCommits)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString sessionDir = QDir(tmp.path()).absoluteFilePath("session");
  nim::ZBlockedAutoTraceSession session(sessionDir);

  nim::ZBlockedAutoTraceManifest manifest;
  manifest.formatVersion = nim::kBlockedAutoTraceManifestFormatVersion;
  manifest.datasetId = "unit_test_dataset";
  manifest.channel = 0;
  manifest.time = 0;
  manifest.signalDownsampleRatio = {1, 1, 1};
  manifest.zScale = 2.5;
  manifest.datasetShape = {.width = 32, .height = 32, .depth = 16};
  manifest.block = {.coreX = 16, .coreY = 16, .coreZ = 16, .halo = 2};
  manifest.subtractConstant = 0.0;
  manifest.traceConfig = nim::TraceConfig{};

  ASSERT_NO_THROW(session.ensureCreatedOrThrow(manifest));

  nim::ZBlockedAutoTraceCommitWrite c1;
  c1.info.formatVersion = nim::kBlockedAutoTraceCommitFormatVersion;
  c1.info.commitId = 1;
  c1.info.blockId = {.bx = 0, .by = 0, .bz = 0};
  c1.info.didSeedScan = true;
  c1.scheduler.nextLinearBlockIndex = 1;
  ASSERT_NO_THROW(session.writeCommitOrThrow(c1));

  // Create an incomplete commit_000002 (no commit.json): must be ignored.
  const QString blocksDir = QDir(sessionDir).absoluteFilePath("blocks");
  const QString incompleteDir = QDir(blocksDir).absoluteFilePath(commitDirName(2));
  ASSERT_TRUE(QDir().mkpath(incompleteDir));
  writeEmptyFileOrThrow(QDir(incompleteDir).absoluteFilePath("swc_delta.swc"));
  // Intentionally omit commit.json.

  // Create a fully-formed commit_000003: should still be ignored since commit_000002 is missing.
  const QString commit3Dir = QDir(blocksDir).absoluteFilePath(commitDirName(3));
  ASSERT_TRUE(QDir().mkpath(commit3Dir));
  {
    json::object info;
    info["format_version"] = 1;
    info["commit_id"] = 3;
    json::object blockId;
    blockId["bx"] = 0;
    blockId["by"] = 0;
    blockId["bz"] = 0;
    info["block_id"] = std::move(blockId);
    info["did_seed_scan"] = false;
    info["new_swc_nodes"] = 0;
    info["pending_tasks"] = 0;
    nim::saveJsonObject(info, QDir(commit3Dir).absoluteFilePath("commit.json"));
  }
  {
    json::object frontier;
    frontier["tasks"] = json::array{};
    nim::saveJsonObject(frontier, QDir(commit3Dir).absoluteFilePath("frontier.json"));
  }
  {
    json::object scheduler;
    scheduler["next_linear_block_index"] = 0;
    nim::saveJsonObject(scheduler, QDir(commit3Dir).absoluteFilePath("scheduler.json"));
  }
  writeEmptyFileOrThrow(QDir(commit3Dir).absoluteFilePath("swc_delta.swc"));

  EXPECT_EQ(session.latestCommittedIdOrZero(), 1u);

  const nim::ZBlockedAutoTraceLoadedState state = session.loadLatestOrEmptyOrThrow();
  EXPECT_EQ(state.commitId, 1u);
}

} // namespace
