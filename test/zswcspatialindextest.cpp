#include <gtest/gtest.h>

#include "zswc.h"
#include "zswcgeometrymaskvolume.h"
#include "zswcspatialindex.h"

namespace {

TEST(ZSwcSpatialIndex, ContainsPointOnCylinder)
{
  nim::ZSwc swc;

  nim::SwcNode root(/*id*/ 1, /*type*/ 0, /*x*/ 0.0, /*y*/ 0.0, /*z*/ 0.0, /*radius*/ 2.0, /*parentID*/ -1);
  nim::SwcNode child(/*id*/ 2, /*type*/ 0, /*x*/ 10.0, /*y*/ 0.0, /*z*/ 0.0, /*radius*/ 2.0, /*parentID*/ 1);

  auto itRoot = swc.appendRoot(root);
  (void)swc.appendChild(itRoot, child);

  nim::ZSwcSpatialIndex idx;
  idx.rebuild(swc);
  EXPECT_FALSE(idx.empty());

  EXPECT_TRUE(idx.containsPoint(0.0, 0.0, 0.0));
  EXPECT_TRUE(idx.containsPoint(5.0, 0.0, 0.0));
  EXPECT_TRUE(idx.containsPoint(10.0, 0.0, 0.0));

  EXPECT_TRUE(idx.containsPoint(5.0, 1.9, 0.0));
  EXPECT_FALSE(idx.containsPoint(5.0, 2.1, 0.0));
}

TEST(ZSwcSpatialIndex, ContainsPointWithTaperedRadius)
{
  nim::ZSwc swc;

  nim::SwcNode root(/*id*/ 1, /*type*/ 0, /*x*/ 0.0, /*y*/ 0.0, /*z*/ 0.0, /*radius*/ 2.0, /*parentID*/ -1);
  nim::SwcNode child(/*id*/ 2, /*type*/ 0, /*x*/ 10.0, /*y*/ 0.0, /*z*/ 0.0, /*radius*/ 4.0, /*parentID*/ 1);

  auto itRoot = swc.appendRoot(root);
  (void)swc.appendChild(itRoot, child);

  nim::ZSwcSpatialIndex idx;
  idx.rebuild(swc);

  // Near the root: radius ~2.0
  EXPECT_FALSE(idx.containsPoint(1.0, 3.0, 0.0));
  // Near the child: radius ~4.0
  EXPECT_TRUE(idx.containsPoint(9.0, 3.0, 0.0));
}

TEST(ZSwcSpatialIndex, ContainsPointRespectsZScale)
{
  nim::ZSwc swc;
  nim::SwcNode root(/*id*/ 1, /*type*/ 0, /*x*/ 0.0, /*y*/ 0.0, /*z*/ 0.0, /*radius*/ 2.0, /*parentID*/ -1);
  (void)swc.appendRoot(root);

  nim::ZSwcSpatialIndex idxIso;
  idxIso.setZScale(1.0);
  idxIso.rebuild(swc);
  EXPECT_TRUE(idxIso.containsPoint(0.0, 0.0, 1.0));
  EXPECT_FALSE(idxIso.containsPoint(0.0, 0.0, 3.0));

  // With coarser Z spacing (zScale>1), the same dz in voxel coordinates corresponds to a larger physical distance,
  // so the tube occupies fewer slices along Z.
  nim::ZSwcSpatialIndex idxCoarseZ;
  idxCoarseZ.setZScale(5.0);
  idxCoarseZ.rebuild(swc);
  EXPECT_FALSE(idxCoarseZ.containsPoint(0.0, 0.0, 1.0));
  EXPECT_TRUE(idxCoarseZ.containsPoint(0.0, 0.0, 0.3));
}

TEST(ZSwcGeometryMaskVolume, ValueAsDoubleMatchesIndex)
{
  nim::ZSwc swc;
  nim::SwcNode root(/*id*/ 1, /*type*/ 0, /*x*/ 5.0, /*y*/ 5.0, /*z*/ 5.0, /*radius*/ 3.0, /*parentID*/ -1);
  (void)swc.appendRoot(root);

  auto idx = std::make_shared<nim::ZSwcSpatialIndex>();
  idx->setZScale(1.0);
  idx->rebuild(swc);

  nim::ZSwcGeometryMaskVolume mask(idx, /*w*/ 16, /*h*/ 16, /*d*/ 16, /*zScale*/ 1.0);

  EXPECT_EQ(mask.valueAsDouble(5, 5, 5), 1.0);
  EXPECT_EQ(mask.valueAsDouble(5, 5, 8), 1.0);
  EXPECT_EQ(mask.valueAsDouble(5, 5, 9), 0.0);

  EXPECT_EQ(mask.valueAsDouble(-1, 5, 5), 0.0);
  EXPECT_EQ(mask.valueAsDouble(5, -1, 5), 0.0);
  EXPECT_EQ(mask.valueAsDouble(5, 5, -1), 0.0);
  EXPECT_EQ(mask.valueAsDouble(100, 5, 5), 0.0);
}

TEST(ZSwcGeometryMaskVolume, ConvertsLegacyMaskZToImageSpace)
{
  nim::ZSwc swc;
  nim::SwcNode root(/*id*/ 1, /*type*/ 0, /*x*/ 5.0, /*y*/ 5.0, /*z*/ 105.0, /*radius*/ 3.0, /*parentID*/ -1);
  (void)swc.appendRoot(root);

  auto idx = std::make_shared<nim::ZSwcSpatialIndex>();
  idx->setZScale(5.0);
  idx->rebuild(swc);

  nim::ZSwcGeometryMaskVolume mask(idx, /*w*/ 16, /*h*/ 16, /*d*/ 64, /*zScale*/ 5.0, glm::dvec3{0.0, 0.0, 100.0});

  EXPECT_EQ(mask.valueAsDouble(5, 5, 25), 1.0);
  EXPECT_EQ(mask.valueAsDouble(5, 5, 29), 0.0);
}

} // namespace
