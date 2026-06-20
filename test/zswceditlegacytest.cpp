#include <gtest/gtest.h>

#include "zswceditlegacy.h"

#include <cstdint>

namespace nim {
namespace {

[[nodiscard]] size_t countRoots(const ZSwc& swc)
{
  size_t roots = 0;
  for (auto it = swc.begin(); it != swc.end(); ++it) {
    if (ZSwc::isRoot(it)) {
      ++roots;
    }
  }
  return roots;
}

[[nodiscard]] SwcNode makeNode(int64_t id, double x, double y, double z = 0.0, double radius = 1.0)
{
  SwcNode node;
  node.id = id;
  node.type = 0;
  node.x = x;
  node.y = y;
  node.z = z;
  node.radius = radius;
  node.parentID = -1;
  node.label = 0;
  return node;
}

TEST(ZSwcEditLegacyLikeTest, DeleteChildrenBecomeRoots)
{
  ZSwc swc;

  SwcNode root;
  root.id = 1;
  root.type = 0;
  root.x = 0.0;
  root.y = 0.0;
  root.z = 0.0;
  root.radius = 1.0;
  root.parentID = -1;
  root.label = 0;
  const auto rootIt = swc.appendRoot(root);

  SwcNode child;
  child.id = 2;
  child.type = 0;
  child.x = 1.0;
  child.y = 0.0;
  child.z = 0.0;
  child.radius = 1.0;
  child.parentID = -1;
  child.label = 0;
  const auto childIt = swc.appendChild(rootIt, child);

  SwcNode grand;
  grand.id = 3;
  grand.type = 0;
  grand.x = 2.0;
  grand.y = 0.0;
  grand.z = 0.0;
  grand.radius = 1.0;
  grand.parentID = -1;
  grand.label = 0;
  const auto grandIt = swc.appendChild(childIt, grand);

  ASSERT_EQ(swc.size(), 3u);
  ASSERT_EQ(countRoots(swc), 1u);

  deleteNodesLegacyLike(swc, {childIt});

  EXPECT_EQ(swc.size(), 2u);
  EXPECT_EQ(countRoots(swc), 2u);

  // Root remains a root.
  bool rootFound = false;
  bool grandFound = false;
  for (auto it = swc.begin(); it != swc.end(); ++it) {
    if (it->id == 1) {
      rootFound = true;
      EXPECT_TRUE(ZSwc::isRoot(it));
    }
    if (it->id == 3) {
      grandFound = true;
      EXPECT_TRUE(ZSwc::isRoot(it));
    }
  }
  EXPECT_TRUE(rootFound);
  EXPECT_TRUE(grandFound);

  static_cast<void>(grandIt);
}

TEST(ZSwcEditLegacyLikeTest, ConnectSelectedNodesConnectsForests)
{
  ZSwc swc;

  SwcNode a;
  a.id = 1;
  a.type = 0;
  a.x = 0.0;
  a.y = 0.0;
  a.z = 0.0;
  a.radius = 1.0;
  a.parentID = -1;
  a.label = 0;
  const auto aIt = swc.appendRoot(a);

  SwcNode b;
  b.id = 2;
  b.type = 0;
  b.x = 10.0;
  b.y = 0.0;
  b.z = 0.0;
  b.radius = 1.0;
  b.parentID = -1;
  b.label = 0;
  const auto bIt = swc.appendRoot(b);

  ASSERT_EQ(countRoots(swc), 2u);

  const bool connected = connectSelectedNodesLegacyLike(swc, {aIt, bIt});
  EXPECT_TRUE(connected);
  EXPECT_EQ(countRoots(swc), 1u);
}

TEST(ZSwcEditLegacyLikeTest, MergeSelectedNodesPreservesExternalParent)
{
  ZSwc swc;

  SwcNode root;
  root.id = 1;
  root.type = 0;
  root.x = 0.0;
  root.y = 0.0;
  root.z = 0.0;
  root.radius = 1.0;
  root.parentID = -1;
  root.label = 0;
  const auto rootIt = swc.appendRoot(root);

  SwcNode n1;
  n1.id = 2;
  n1.type = 0;
  n1.x = 1.0;
  n1.y = 0.0;
  n1.z = 0.0;
  n1.radius = 1.0;
  n1.parentID = -1;
  n1.label = 0;
  const auto n1It = swc.appendChild(rootIt, n1);

  SwcNode n2;
  n2.id = 3;
  n2.type = 0;
  n2.x = 3.0;
  n2.y = 0.0;
  n2.z = 0.0;
  n2.radius = 2.0;
  n2.parentID = -1;
  n2.label = 0;
  const auto n2It = swc.appendChild(n1It, n2);

  ASSERT_EQ(swc.size(), 3u);
  ASSERT_EQ(countRoots(swc), 1u);

  const auto merged = mergeSelectedNodesLegacyLike(swc, {n1It, n2It});
  ASSERT_TRUE(merged.has_value());

  EXPECT_EQ(swc.size(), 2u);
  EXPECT_EQ(countRoots(swc), 1u);

  // The new core node is attached under the original external parent (root).
  const auto core = *merged;
  EXPECT_FALSE(ZSwc::isNull(core));
  EXPECT_FALSE(ZSwc::isRoot(core));
  EXPECT_EQ(ZSwc::parent(core), rootIt);

  // Core geometry matches the legacy construction: centroid position, max radius.
  EXPECT_DOUBLE_EQ(core->x, (1.0 + 3.0) * 0.5);
  EXPECT_DOUBLE_EQ(core->y, 0.0);
  EXPECT_DOUBLE_EQ(core->z, 0.0);
  EXPECT_DOUBLE_EQ(core->radius, 2.0);
}

TEST(ZSwcEditLegacyLikeTest, ResolveCrossoverPairsOppositeOrthogonalBranches)
{
  ZSwc swc;

  const auto center = swc.appendRoot(makeNode(1, 0.0, 0.0));
  const auto east = swc.appendChild(center, makeNode(2, 1.0, 0.0));
  const auto west = swc.appendChild(center, makeNode(3, -1.0, 0.0));
  const auto north = swc.appendChild(center, makeNode(4, 0.0, 1.0));
  const auto south = swc.appendChild(center, makeNode(5, 0.0, -1.0));

  EXPECT_TRUE(resolveCrossoverLegacyLike(swc, center));

  EXPECT_EQ(swc.size(), 4u);
  EXPECT_EQ(countRoots(swc), 2u);
  EXPECT_TRUE((ZSwc::parent(east) == west && ZSwc::isRoot(west)) || (ZSwc::parent(west) == east && ZSwc::isRoot(east)));
  EXPECT_TRUE((ZSwc::parent(north) == south && ZSwc::isRoot(south)) ||
              (ZSwc::parent(south) == north && ZSwc::isRoot(north)));
}

} // namespace
} // namespace nim
