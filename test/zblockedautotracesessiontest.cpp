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
#include <unordered_map>
#include <vector>

namespace {

[[nodiscard]] QString commitDirName(uint64_t commitId)
{
  return QStringLiteral("commit_%1").arg(static_cast<qulonglong>(commitId), 6, 10, QLatin1Char('0'));
}

void applyDeltaToSwc(const std::vector<nim::ZBlockedAutoTraceSwcDeltaNode>& delta, nim::ZSwc& swc)
{
  std::unordered_map<int64_t, nim::ZSwc::SwcTreeNode> nodeById;
  nodeById.reserve(swc.size() * 2 + delta.size() * 2 + 1);
  for (auto it = swc.begin(); it != swc.end(); ++it) {
    nodeById.emplace(it->id, it);
  }

  std::vector<std::pair<nim::ZSwc::SwcTreeNode, int64_t>> attach;
  attach.reserve(delta.size());
  for (const auto& node : delta) {
    nim::SwcNode swcNode(node.id, node.type, node.x, node.y, node.z, node.radius, node.parentId);
    auto it = swc.appendRoot(swcNode);
    nodeById.emplace(node.id, it);
    attach.emplace_back(it, node.parentId);
  }

  for (const auto& [child, parentId] : attach) {
    if (parentId < 0) {
      continue;
    }
    swc.appendChild(nodeById.at(parentId), child);
  }
}

void writeCommitWithSnapshotOrThrow(nim::ZBlockedAutoTraceSession& session,
                                    const nim::ZBlockedAutoTraceCommitWrite& commit,
                                    const nim::ZSwc& swcSnapshot,
                                    const std::vector<nim::ZBlockedAutoTraceBlockId>& seedScannedBlocks)
{
  ASSERT_NO_THROW(session.writeCommitOrThrow(commit, swcSnapshot, seedScannedBlocks));
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
  manifest.zToXYRatio = 2.5;
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

  nim::ZSwc swcAfterC1;
  applyDeltaToSwc(c1.swcDeltaNodes, swcAfterC1);
  const std::vector<nim::ZBlockedAutoTraceBlockId> seedScannedAfterC1{
    {.bx = 0, .by = 0, .bz = 0}
  };
  writeCommitWithSnapshotOrThrow(session, c1, swcAfterC1, seedScannedAfterC1);

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

  nim::ZSwc swcAfterC2 = swcAfterC1;
  applyDeltaToSwc(c2.swcDeltaNodes, swcAfterC2);
  writeCommitWithSnapshotOrThrow(session, c2, swcAfterC2, seedScannedAfterC1);

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

TEST(ZBlockedAutoTraceSession, RollingSwcMirrorsLatestSelfContainedCommit)
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
  manifest.zToXYRatio = 2.5;
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
  nim::ZSwc swcAfterC1;
  applyDeltaToSwc(c1.swcDeltaNodes, swcAfterC1);
  const std::vector<nim::ZBlockedAutoTraceBlockId> seedScannedAfterC1{
    {.bx = 0, .by = 0, .bz = 0}
  };
  writeCommitWithSnapshotOrThrow(session, c1, swcAfterC1, seedScannedAfterC1);

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
  nim::ZSwc swcAfterC2 = swcAfterC1;
  applyDeltaToSwc(c2.swcDeltaNodes, swcAfterC2);
  const std::vector<nim::ZBlockedAutoTraceBlockId> seedScannedAfterC2{
    {.bx = 0, .by = 0, .bz = 0},
    {.bx = 1, .by = 0, .bz = 0},
  };
  writeCommitWithSnapshotOrThrow(session, c2, swcAfterC2, seedScannedAfterC2);

  // The rolling artifact should mirror the latest committed self-contained snapshot directly.
  ASSERT_NO_THROW(session.updateRollingSwcFromCommitOrThrow(2));

  const QString rollingPath = QDir(sessionDir).absoluteFilePath("result_tracing.swc");
  ASSERT_TRUE(QFileInfo::exists(rollingPath)) << rollingPath;

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
  EXPECT_EQ(ids[0], 1);
  EXPECT_EQ(ids[1], 2);
  EXPECT_EQ(ids[2], 3);

  const QString fullPath = QDir(sessionDir).absoluteFilePath("blocks/commit_000002/swc_full.swc");
  ASSERT_TRUE(QFileInfo::exists(fullPath)) << fullPath;
  EXPECT_EQ(QFileInfo(rollingPath).size(), QFileInfo(fullPath).size());

  const QString statePath = QDir(sessionDir).absoluteFilePath("result_tracing_state.json");
  EXPECT_FALSE(QFileInfo::exists(statePath)) << statePath;
}

TEST(ZBlockedAutoTraceSession, SelfContainedCommitResumesAfterEarlierDeletionAndLaterCorruption)
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
  manifest.zToXYRatio = 2.5;
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
  c1.swcDeltaNodes.push_back(nim::ZBlockedAutoTraceSwcDeltaNode{
    .id = 1,
    .type = 0,
    .x = 0.0,
    .y = 0.0,
    .z = 0.0,
    .radius = 2.0,
    .parentId = -1,
  });
  nim::ZSwc swcAfterC1;
  applyDeltaToSwc(c1.swcDeltaNodes, swcAfterC1);
  const std::vector<nim::ZBlockedAutoTraceBlockId> seedScannedAfterC1{
    {.bx = 0, .by = 0, .bz = 0}
  };
  writeCommitWithSnapshotOrThrow(session, c1, swcAfterC1, seedScannedAfterC1);

  nim::ZBlockedAutoTraceCommitWrite c2;
  c2.info.formatVersion = nim::kBlockedAutoTraceCommitFormatVersion;
  c2.info.commitId = 2;
  c2.info.blockId = {.bx = 1, .by = 0, .bz = 0};
  c2.info.didSeedScan = true;
  c2.scheduler.nextLinearBlockIndex = 2;
  c2.swcDeltaNodes.push_back(nim::ZBlockedAutoTraceSwcDeltaNode{
    .id = 2,
    .type = 0,
    .x = 1.0,
    .y = 0.0,
    .z = 0.0,
    .radius = 1.0,
    .parentId = 1,
  });
  nim::ZSwc swcAfterC2 = swcAfterC1;
  applyDeltaToSwc(c2.swcDeltaNodes, swcAfterC2);
  const std::vector<nim::ZBlockedAutoTraceBlockId> seedScannedAfterC2{
    {.bx = 0, .by = 0, .bz = 0},
    {.bx = 1, .by = 0, .bz = 0},
  };
  writeCommitWithSnapshotOrThrow(session, c2, swcAfterC2, seedScannedAfterC2);

  // Simulate a broken later commit directory: it has a valid commit marker but is missing the self-contained snapshot.
  const QString blocksDir = QDir(sessionDir).absoluteFilePath("blocks");
  const QString commit3Dir = QDir(blocksDir).absoluteFilePath(commitDirName(3));
  ASSERT_TRUE(QDir().mkpath(commit3Dir));
  {
    json::object info;
    info["format_version"] = nim::kBlockedAutoTraceCommitFormatVersion;
    info["commit_id"] = 3;
    json::object blockId;
    blockId["bx"] = 2;
    blockId["by"] = 0;
    blockId["bz"] = 0;
    info["block_id"] = std::move(blockId);
    info["did_seed_scan"] = true;
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
    scheduler["next_linear_block_index"] = 3;
    nim::saveJsonObject(scheduler, QDir(commit3Dir).absoluteFilePath("scheduler.json"));
  }
  // Intentionally omit swc_full.swc and seed_scanned_blocks.json. `swc_delta.swc` is a debug/rolling artifact and
  // should not be required for resume either, so we omit it too.

  const nim::ZBlockedAutoTraceLoadedState stateWithBrokenLaterCommit = session.loadLatestOrEmptyOrThrow();
  EXPECT_EQ(stateWithBrokenLaterCommit.commitId, 2u);
  EXPECT_EQ(stateWithBrokenLaterCommit.swc.size(), 2u);
  ASSERT_EQ(stateWithBrokenLaterCommit.seedScannedBlocks.size(), 2u);

  // Losing an earlier commit directory must not prevent resume from the latest good self-contained commit.
  ASSERT_TRUE(QDir(QDir(blocksDir).absoluteFilePath(commitDirName(1))).removeRecursively());

  const nim::ZBlockedAutoTraceLoadedState stateAfterEarlierDeletion = session.loadLatestOrEmptyOrThrow();
  EXPECT_EQ(stateAfterEarlierDeletion.commitId, 2u);
  EXPECT_EQ(stateAfterEarlierDeletion.swc.size(), 2u);
  ASSERT_EQ(stateAfterEarlierDeletion.seedScannedBlocks.size(), 2u);
  EXPECT_EQ(stateAfterEarlierDeletion.seedScannedBlocks[0].bx, 0);
  EXPECT_EQ(stateAfterEarlierDeletion.seedScannedBlocks[1].bx, 1);
}

} // namespace
