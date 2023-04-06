#include "zimgregionalextrema.h"

#include "zimgneighborhooditerator.h"
#include <stack>

namespace nim {

template<bool ReportProgress>
template<template<typename> class Compare>
ZImg ZImgRegionalExtrema<ReportProgress>::regionalExtrema(const ZImg& img, size_t conn)
{
  if (conn == 0) {
    conn = img.is2DImg() ? 8 : 26;
  } else if (conn != 4 && conn != 8 && conn != 6 && conn != 18 && conn != 26) {
    throw ZException(QString("invalid conn input: %1").arg(conn));
  }
  if (img.is2DImg() && conn != 4 && conn != 8) {
    if (conn == 6) {
      conn = 4;
    } else {
      conn = 8;
    }
  }

  ZImg res;
  if (img.isEmpty()) {
    return res;
  }
  ZImgInfo info = img.info();
  info.bytesPerVoxel = 1;
  info.voxelFormat = VoxelFormat::Unsigned;
  res = ZImg(info);
  res.fill(1);

  IMG_TYPED_CALL_FIX2NDTYPE(regionalExtrema_Impl, img.info(), Compare, res, img, conn)

  return res;
}

template<bool ReportProgress>
template<typename TVoxel, template<typename> class Compare>
void ZImgRegionalExtrema<ReportProgress>::regionalExtrema_Impl(ZImg& res, const ZImg& img, size_t conn)
{
  Compare<TVoxel> compare;
  for (size_t t = 0; t < img.numTimes(); ++t) {
    TVoxel extreme;
    if (compare(1, 0)) {
      extreme = img.dataRangeMin<TVoxel>();
    } else {
      extreme = img.dataRangeMax<TVoxel>();
    }
    uint8_t* out = res.timeData<uint8_t>(t);
    const TVoxel* data = img.timeData<TVoxel>(t);
    double voxelNumber = img.timeVoxelNumber();

    std::stack<size_t, std::vector<size_t>> stk;
    ZImgNeighborhoodConstIterator<TVoxel> nit = ZImgNeighborhoodConstIterator<TVoxel>(ZNeighborhood(conn), img);

    TVoxel centerValue;
    size_t idx;
    size_t n;
    for (; !nit.isAtEnd(); ++nit) {
      idx = nit.index();
      centerValue = data[idx];

      this->reportProgress(idx / voxelNumber);

      if (centerValue == extreme) {
        out[idx] = 0;
      } else {
        // check each neighbor
        for (n = 0; n < conn; ++n) {
          if (nit.isInBound(n) && compare(data[nit.index(n)], centerValue)) {
            // The centre pixel cannot be part of a regional maxima
            // because one of its neighbors is higher.
            // push into stack to process later
            stk.push(idx);
            out[idx] = 0;
            break;
          }
        }
      }
    }

    while (!stk.empty()) {
      // position the iterator
      nit.goToIndex(stk.top());
      centerValue = data[nit.index()];
      stk.pop();

      // check neighbors
      for (n = 0; n < conn; ++n) {
        if (nit.isInBound(n)) {
          idx = nit.index(n);
          if (out[idx] != 0 && !compare(data[idx], centerValue)) {
            stk.push(idx);
            out[idx] = 0;
          }
        }
      }
    }
  }
  this->reportProgress(1.0);
}

template class ZImgRegionalExtrema<true>;

template class ZImgRegionalExtrema<false>;

template ZImg ZImgRegionalExtrema<true>::regionalExtrema<std::greater>(const ZImg&, size_t);

template ZImg ZImgRegionalExtrema<true>::regionalExtrema<std::less>(const ZImg&, size_t);

template ZImg ZImgRegionalExtrema<false>::regionalExtrema<std::greater>(const ZImg&, size_t);

template ZImg ZImgRegionalExtrema<false>::regionalExtrema<std::less>(const ZImg&, size_t);

} // namespace nim
