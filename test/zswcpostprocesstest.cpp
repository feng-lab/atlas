#include <gtest/gtest.h>

#include "zswcpostprocess.h"

namespace nim {
namespace {

[[nodiscard]] SwcNode makeNode(int64_t id, double x, double y, double z)
{
  SwcNode n;
  n.id = id;
  n.type = 0;
  n.x = x;
  n.y = y;
  n.z = z;
  n.radius = 1.0;
  n.parentID = -1;
  n.label = 0;
  return n;
}

void appendChain(ZSwc& swc, ZSwc::SwcTreeNode root, int edges, double dx, int64_t& nextId)
{
  auto parent = root;
  for (int i = 1; i <= edges; ++i) {
    const auto child =
      swc.appendChild(parent, makeNode(nextId++, root->x + static_cast<double>(i) * dx, root->y, root->z));
    parent = child;
  }
}

TEST(ZSwcPostprocessLegacyLikeTest, RemoveOrphanBlob_MinLength0_ComputesMeanAndReversesRootOrder)
{
  ZSwc swc;

  // Construct 10 roots with trunk lengths 1..10 (unit edge lengths).
  // With minLength=0 and roots>=minOrphanCount, the pruning threshold is the mean trunk length (5.5),
  // so roots with trunk length >= 5.5 (6..10) are kept, and then the root list is reversed.
  int64_t nextId = 1000;
  for (int length = 1; length <= 10; ++length) {
    const auto root = swc.appendRoot(makeNode(length, 0.0, static_cast<double>(length) * 10.0, 0.0));
    appendChain(swc, root, /*edges*/ length, /*dx*/ 1.0, nextId);
  }

  ASSERT_EQ(swc.numRoots(), 10u);

  swcTreeRemoveOrphanBlobLegacyLike(swc, /*minLength*/ 0.0, /*minOrphanCount*/ 10);

  EXPECT_EQ(swc.numRoots(), 5u);

  std::vector<int64_t> rootIds;
  for (auto it = swc.beginRoot(); it != swc.endRoot(); ++it) {
    rootIds.push_back(it->id);
  }

  EXPECT_EQ(rootIds, (std::vector<int64_t>{10, 9, 8, 7, 6}));
}

TEST(ZSwcPostprocessLegacyLikeTest, RemoveOrphanBlob_ExplicitMinLength_PruningUsesTrunkDiameter)
{
  ZSwc swc;
  int64_t nextId = 2000;

  // Root A: two branches of length 10 and 1, diameter is 11.
  const auto rootA = swc.appendRoot(makeNode(/*id*/ 1, 0.0, 0.0, 0.0));
  appendChain(swc, rootA, /*edges*/ 10, /*dx*/ 1.0, nextId);
  static_cast<void>(swc.appendChild(rootA, makeNode(nextId++, -1.0, 0.0, 0.0)));

  // Root B: chain of length 9, diameter is 9.
  const auto rootB = swc.appendRoot(makeNode(/*id*/ 2, 0.0, 100.0, 0.0));
  appendChain(swc, rootB, /*edges*/ 9, /*dx*/ 1.0, nextId);

  ASSERT_EQ(swc.numRoots(), 2u);

  // Keep only roots with diameter >= 10.0, so A survives and B is removed.
  swcTreeRemoveOrphanBlobLegacyLike(swc, /*minLength*/ 10.0, /*minOrphanCount*/ 10);

  EXPECT_EQ(swc.numRoots(), 1u);
  ASSERT_NE(swc.beginRoot(), swc.endRoot());
  EXPECT_EQ(swc.beginRoot()->id, 1);
}

} // namespace
} // namespace nim
