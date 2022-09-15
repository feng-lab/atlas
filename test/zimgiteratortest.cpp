#include "zimg.h"
#include "zimgregioniterator.h"
#include "zimgneighborhooditerator.h"
#include "zimgneighborhoodwithcoorditerator.h"
#include "zimgneighborhoodwithptriterator.h"
#include "ztest.h"

TEST(ImgIterator, region)
{
  using namespace nim;
  ZImgInfo info(100, 100, 20);
  info.bytesPerVoxel = 4;
  ZImg img(info);
  uint32_t* data = img.timeData<uint32_t>(0);
  for (size_t i=0; i<img.timeVoxelNumber(); ++i)
    data[i] = i;

  ZImgRegion region(10, 20, 10, 20, 10, 20);
  region.resolveRegionEnd(info);
  int64_t idx = 0;
  for (ZImgRegionConstIterator<uint32_t> it = ZImgRegionConstIterator<uint32_t>(img, region);
       !it.isAtEnd(); ++it) {
    if (idx == 0) {
      ASSERT_EQ(*img.data<uint32_t>(region.start), *it);
    } else if (idx == 999) {
      ASSERT_EQ(*img.data<uint32_t>(region.end - 1), *it);
    } else {
      ASSERT_EQ(idx, it.index());
      ZVoxelCoordinate coord = it.coord();
      ASSERT_TRUE(coord.allGreaterEqual(region.start));
      ASSERT_TRUE(coord.allLessThan(region.end));
      ASSERT_EQ(*img.data<uint32_t>(coord), *it);
    }
    ++idx;
  }

  region = ZImgRegion(30, 40, 30, 40, 1, 11);
  region.resolveRegionEnd(info);
  idx = 0;
  for (ZImgRegionConstIterator<uint32_t> it = ZImgRegionConstIterator<uint32_t>(img, region);
       !it.isAtEnd(); ++it) {
    if (idx == 0) {
      ASSERT_EQ(*img.data<uint32_t>(region.start), *it);
    } else if (idx == 999) {
      ASSERT_EQ(*img.data<uint32_t>(region.end - 1), *it);
    } else {
      ASSERT_EQ(idx, it.index());
      ZVoxelCoordinate coord = it.coord();
      ASSERT_TRUE(coord.allGreaterEqual(region.start));
      ASSERT_TRUE(coord.allLessThan(region.end));
      ASSERT_EQ(*img.data<uint32_t>(coord), *it);
    }
    ++idx;
  }

  region = ZImgRegion(60, 90, 20, 40, 1, 15);
  region.resolveRegionEnd(info);
  idx = 0;
  int64_t numVoxel = (90-60)*(20)*14;
  ZImgRegionConstIterator<uint32_t> it = ZImgRegionConstIterator<uint32_t>(img, region);
  for (it.goToLast(); !it.isBeforeBegin(); --it) {
    if (idx == 0) {
      ASSERT_EQ(*img.data<uint32_t>(region.end - 1), *it);
    } else if (idx == numVoxel - 1) {
      ASSERT_EQ(*img.data<uint32_t>(region.start), *it);
    } else {
      ASSERT_EQ(numVoxel-1-idx, it.index());
      ZVoxelCoordinate coord = it.coord();
      ASSERT_TRUE(coord.allGreaterEqual(region.start));
      ASSERT_TRUE(coord.allLessThan(region.end));
      ASSERT_EQ(*img.data<uint32_t>(coord), *it);
    }
    ++idx;
  }

  for (auto lit = ZImgRegionIterator<uint32_t>(img, region); !lit.isAtEnd(); ++lit) {
    *lit = 32;
  }

  for (auto lit = ZImgRegionIterator<uint32_t>(img, region); !lit.isAtEnd(); ++lit) {
    ASSERT_EQ(32_u32, *lit);
  }

  for (auto lit = ZImgRegionIterator<uint32_t>(img, region); !lit.isAtEnd(); lit += 3) {
    *lit = 33;
  }

  for (auto lit = ZImgRegionIterator<uint32_t>(img, region); !lit.isAtEnd(); ++lit) {
    if (lit.index() % 3 == 0)
      ASSERT_EQ(33_u32, *lit);
    else
      ASSERT_EQ(32_u32, *lit);
  }
}

TEST(ImgIterator, neighborhood)
{
  using namespace nim;
  ZImgInfo info(100, 100, 20);
  info.bytesPerVoxel = 4;
  ZImg img(info);
  uint32_t* data = img.timeData<uint32_t>(0);
  for (size_t i=0; i<img.timeVoxelNumber(); ++i)
    data[i] = i;

  ZImgRegion region(10, 20, 10, 20, 10, 20);
  region.resolveRegionEnd(info);
  int64_t idx = 0;
  ZImgNeighborhoodConstIterator<uint32_t> it = ZImgNeighborhoodConstIterator<uint32_t>(ZNeighborhood(18), img, region);
  for (; !it.isAtEnd(); ++it) {
    if (idx == 0) {
      ASSERT_EQ(*img.data<uint32_t>(region.start), *it);
    } else if (idx == 999) {
      ASSERT_EQ(*img.data<uint32_t>(region.end - 1), *it);
    } else {
      ASSERT_EQ(idx, it.index());
      ZVoxelCoordinate coord = it.coord();
      ASSERT_TRUE(coord.allGreaterEqual(region.start));
      ASSERT_TRUE(coord.allLessThan(region.end));
      ASSERT_EQ(*img.data<uint32_t>(coord), *it);
    }
    ZVoxelCoordinate coord = it.coord();
    ASSERT_EQ(10000_u32, *it - it.valueRef(2));
    if (coord.z == 19) {
      for (size_t i=0; i<it.numNeighbors(); ++i) {
        if (i < 13)
          ASSERT_TRUE(it.isInBound(i));
        else
          ASSERT_FALSE(it.isInBound(i));
      }
    } else {
      for (size_t i=0; i<it.numNeighbors(); ++i) {
        ASSERT_TRUE(it.isInBound(i));
      }
    }
    ++idx;
  }

  idx = 0;
  it.setNeighborhood(ZNeighborhood(1,1,1,1,1,0,false));
  for (it.goToBegin(); !it.isAtEnd(); ++it) {
    if (idx == 0) {
      ASSERT_EQ(*img.data<uint32_t>(region.start), *it);
    } else if (idx == 999) {
      ASSERT_EQ(*img.data<uint32_t>(region.end - 1), *it);
    } else {
      ASSERT_EQ(idx, it.index());
      ZVoxelCoordinate coord = it.coord();
      ASSERT_TRUE(coord.allGreaterEqual(region.start));
      ASSERT_TRUE(coord.allLessThan(region.end));
      ASSERT_EQ(*img.data<uint32_t>(coord), *it);
    }
    ASSERT_EQ(10000_u32, *it - it.valueRef(4));
    for (size_t i=0; i<it.numNeighbors(); ++i) {
      ASSERT_TRUE(it.isInBound(i));
    }
    ++idx;
  }

  ZImg emptyImg;
  ZImgNeighborhoodConstIterator<uint8_t> eit = ZImgNeighborhoodConstIterator<uint8_t>(ZNeighborhood(18), emptyImg);
  for (; !eit.isAtEnd(); ++eit) {
    ASSERT_EQ(15, *eit); // should not be called
  }
  for (eit.goToBegin(); !eit.isAtEnd(); ++eit) {
    ASSERT_EQ(15, *eit); // should not be called
  }
}

TEST(ImgIterator, neighborhoodWithPtr)
{
  using namespace nim;
  ZImgInfo info(100, 100, 20);
  info.bytesPerVoxel = 4;
  ZImg img(info);
  uint32_t* data = img.timeData<uint32_t>(0);
  for (size_t i=0; i<img.timeVoxelNumber(); ++i)
    data[i] = i;

  ZImgRegion region(10, 20, 10, 20, 10, 20);
  region.resolveRegionEnd(info);
  int64_t idx = 0;
  ZImgNeighborhoodWithPtrConstIterator<uint32_t> it = ZImgNeighborhoodWithPtrConstIterator<uint32_t>(ZNeighborhood(18), img, region);
  for (; !it.isAtEnd(); ++it) {
    if (idx == 0) {
      ASSERT_EQ(*img.data<uint32_t>(region.start), *it);
    } else if (idx == 999) {
      ASSERT_EQ(*img.data<uint32_t>(region.end - 1), *it);
    } else {
      ASSERT_EQ(idx, it.index());
      ZVoxelCoordinate coord = it.coord();
      ASSERT_TRUE(coord.allGreaterEqual(region.start));
      ASSERT_TRUE(coord.allLessThan(region.end));
      ASSERT_EQ(*img.data<uint32_t>(coord), *it);
    }
    ZVoxelCoordinate coord = it.coord();
    ZVoxelCoordinate nb1 = it.coord(2);
    ASSERT_EQ(ZVoxelCoordinate(0,0,1), coord - nb1) << qUtf8Printable(coord.toQString()) << qUtf8Printable(nb1.toQString());
    ASSERT_EQ(10000_u32, *it - it.valueRef(2)) << *it << " " << it.valueRef(2)
                                                       << qUtf8Printable(coord.toQString()) << qUtf8Printable(nb1.toQString())
                                                       << (&(*it) - it.valuePtr(2));
    if (coord.z == 19) {
      for (size_t i=0; i<it.numNeighbors(); ++i) {
        if (i < 13)
          ASSERT_TRUE(it.isInBound(i));
        else
          ASSERT_FALSE(it.isInBound(i));
      }
    } else {
      for (size_t i=0; i<it.numNeighbors(); ++i) {
        ASSERT_TRUE(it.isInBound(i));
      }
    }
    ++idx;
  }

  idx = 0;
  it.setNeighborhood(ZNeighborhood(1,1,1,1,1,0,false));
  for (it.goToBegin(); !it.isAtEnd(); ++it) {
    if (idx == 0) {
      ASSERT_EQ(*img.data<uint32_t>(region.start), *it);
    } else if (idx == 999) {
      ASSERT_EQ(*img.data<uint32_t>(region.end - 1), *it);
    } else {
      ASSERT_EQ(idx, it.index());
      ZVoxelCoordinate coord = it.coord();
      ASSERT_TRUE(coord.allGreaterEqual(region.start));
      ASSERT_TRUE(coord.allLessThan(region.end));
      ASSERT_EQ(*img.data<uint32_t>(coord), *it);
    }
    ZVoxelCoordinate coord = it.coord();
    ZVoxelCoordinate nb1 = it.coord(4);
    ASSERT_EQ(ZVoxelCoordinate(0,0,1), coord - nb1);
    ASSERT_EQ(10000_u32, *it - it.valueRef(4));
    for (size_t i=0; i<it.numNeighbors(); ++i) {
      ASSERT_TRUE(it.isInBound(i));
    }
    ++idx;
  }
}

TEST(ImgIterator, neighborhoodWithCoord)
{
  using namespace nim;
  ZImgInfo info(100, 100, 20);
  info.bytesPerVoxel = 4;
  ZImg img(info);
  uint32_t* data = img.timeData<uint32_t>(0);
  for (size_t i=0; i<img.timeVoxelNumber(); ++i)
    data[i] = i;

  ZImgRegion region(10, 20, 10, 20, 10, 20);
  region.resolveRegionEnd(info);
  int64_t idx = 0;
  ZImgNeighborhoodWithCoordConstIterator<uint32_t> it = ZImgNeighborhoodWithCoordConstIterator<uint32_t>(ZNeighborhood(18), img, region);
  for (; !it.isAtEnd(); ++it) {
    if (idx == 0) {
      ASSERT_EQ(*img.data<uint32_t>(region.start), *it);
    } else if (idx == 999) {
      ASSERT_EQ(*img.data<uint32_t>(region.end - 1), *it);
    } else {
      ASSERT_EQ(idx, it.index());
      ZVoxelCoordinate coord = it.coord();
      ASSERT_TRUE(coord.allGreaterEqual(region.start));
      ASSERT_TRUE(coord.allLessThan(region.end));
      ASSERT_EQ(*img.data<uint32_t>(coord), *it);
    }
    ZVoxelCoordinate coord = it.coord();
    ZVoxelCoordinate nb1 = it.coord(2);
    ASSERT_EQ(ZVoxelCoordinate(0,0,1), coord - nb1) << qUtf8Printable(coord.toQString()) << qUtf8Printable(nb1.toQString());
    ASSERT_EQ(10000_u32, *it - it.valueRef(2));
    if (coord.z == 19) {
      for (size_t i=0; i<it.numNeighbors(); ++i) {
        if (i < 13)
          ASSERT_TRUE(it.isInBound(i));
        else
          ASSERT_FALSE(it.isInBound(i));
      }
    } else {
      for (size_t i=0; i<it.numNeighbors(); ++i) {
        ASSERT_TRUE(it.isInBound(i));
      }
    }
    ++idx;
  }

  idx = 0;
  it.setNeighborhood(ZNeighborhood(1,1,1,1,1,0,false));
  for (it.goToBegin(); !it.isAtEnd(); ++it) {
    if (idx == 0) {
      ASSERT_EQ(*img.data<uint32_t>(region.start), *it);
    } else if (idx == 999) {
      ASSERT_EQ(*img.data<uint32_t>(region.end - 1), *it);
    } else {
      ASSERT_EQ(idx, it.index());
      ZVoxelCoordinate coord = it.coord();
      ASSERT_TRUE(coord.allGreaterEqual(region.start));
      ASSERT_TRUE(coord.allLessThan(region.end));
      ASSERT_EQ(*img.data<uint32_t>(coord), *it);
    }
    ZVoxelCoordinate coord = it.coord();
    ZVoxelCoordinate nb1 = it.coord(4);
    ASSERT_EQ(ZVoxelCoordinate(0,0,1), coord - nb1);
    ASSERT_EQ(10000_u32, *it - it.valueRef(4));
    for (size_t i=0; i<it.numNeighbors(); ++i) {
      ASSERT_TRUE(it.isInBound(i));
    }
    ++idx;
  }
}

