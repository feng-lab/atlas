#include "zimgautothreshold.h"

#include "zimgconnectedcomponents.h"
#include "zimgregionalextrema.h"
#include "zbenchtimer.h"

namespace nim {

template<bool ReportProgress>
template<typename TVoxel>
TVoxel ZImgAutoThreshold<ReportProgress>::typedTriangleThre(const ZImg& imgIn, size_t c, size_t t)
{
  if (!imgIn.isType<TVoxel>()) {
    throw ZImgException("input img voxel type doesnot match provided type");
  }

  size_t conn = 18;
  ZImg img = imgIn.createView(c, t);

  this->clearRegisteredSubOperations();
  this->setTotalSubOperationWeight(.8);

  //ZBenchTimer bt;
  //bt.start();
  ZImgRegionalExtrema<ReportProgress> regionalExtrema;
  this->registerSubOperation(&regionalExtrema, .4);
  ZImg locmaxMask = regionalExtrema.regionalMax(img, conn);
  //bt.stop();
  //LOG(INFO) << bt;

  //bt.reset();
  //bt.start();
  ZImgConnectedComponents<ReportProgress> conncomp;
  this->registerSubOperation(&conncomp, .4);
  ConnComp CC = conncomp.runLabelModifyInput(locmaxMask, conn);
  //bt.stop();
  //LOG(INFO) << bt;

  locmaxMask.fill(0);
  uint8_t* locmaxMaskData = locmaxMask.timeData<uint8_t>(0);
  for (size_t i = 0; i < CC.voxelIdxList.size(); ++i) {
    locmaxMaskData[CC.voxelIdxList[i][0]] = 1;
  }
  CC.voxelIdxList.clear();

  std::vector<size_t> hist = img.histogram(0, locmaxMask);
  locmaxMask.clear();

  this->reportProgress(.8);

  size_t low, high;
  histNonZeroRange(hist, low, high);

  if (low == high) { // flat img, return smallest possible value as threshold
    this->reportProgress(1.0);
    return img.dataRangeMin<TVoxel>();
  }

  // search in range [low, high-1]
  // maximum position
  size_t maxIndex = low;
  for (size_t i = low + 1; i < high; ++i) {
    if (hist[i] > hist[maxIndex]) {
      maxIndex = i;
    }
  }
  // minimum position
  size_t minIndex = high - 1;
  while (minIndex > maxIndex && hist[minIndex] == 0)
    --minIndex;

  for (size_t i = minIndex; i-- > maxIndex;) {
    if (hist[i] && hist[i] < hist[minIndex]) {
      minIndex = i;
    }
  }

  if (maxIndex == minIndex) {
    this->reportProgress(1.0);
    return saturate_cast<TVoxel>(img.binRange(low - 1, hist.size()).first);
  }

  // normalize the histogram
  std::vector<double> nhist(minIndex - maxIndex + 1);
  double scale = (nhist.size() - 1.0) / (hist[maxIndex] * 1.0 - hist[minIndex]);
  for (size_t i = 0; i < nhist.size(); ++i) {
    nhist[i] = (hist[i + maxIndex] - hist[minIndex]) * scale;
  }

  size_t threBin = 0;
  double bestScore = nhist[0];

  for (size_t i = 1; i < nhist.size(); ++i) {
    if (nhist[i] > 0) {
      double score = nhist[i] + i;
      if (score < bestScore) {
        bestScore = score;
        threBin = i;
      }
    }
  }
  threBin += maxIndex;

  this->reportProgress(1.0);
  return saturate_cast<TVoxel>(img.binRange(threBin, hist.size()).first);
}

template<bool ReportProgress>
template<typename TVoxel>
TVoxel ZImgAutoThreshold<ReportProgress>::typedCentroidThre(double& cent1, double& cent2, const ZImg& imgIn, size_t c,
                                                            size_t t)
{
  if (!imgIn.isType<TVoxel>()) {
    throw ZImgException("input img voxel type doesnot match provided type");
  }

  ZImg img = imgIn.createView(c, t);
  std::vector<size_t> hist = img.histogram();

  this->reportProgress(.8);

  size_t low, high;
  histNonZeroRange(hist, low, high);

  if (low == high) { // flat img, return smallest possible value as threshold
    this->reportProgress(1.0);
    cent1 = 0;
    cent2 = low;
    std::pair<double, double> cent1Range = img.binRange(std::floor(cent1), hist.size());
    cent1 = cent1Range.first + (cent1Range.second - cent1Range.first) * (cent1 - std::floor(cent1));
    std::pair<double, double> cent2Range = img.binRange(std::floor(cent2), hist.size());
    cent2 = cent2Range.first + (cent2Range.second - cent2Range.first) * (cent2 - std::floor(cent2));
    return img.dataRangeMin<TVoxel>();
  }

  size_t threBin = (low + high) / 2;

  std::vector<size_t> weightPos = hist;
  for (size_t i = low; i <= high; ++i)
    weightPos[i] *= i;

  size_t prevThreBin;
  do {
    if (threBin == high) {
      break;
    }

    prevThreBin = threBin;

    double totalPos = std::accumulate(weightPos.begin() + low, weightPos.begin() + threBin + 1, .0);
    size_t totalWeight = std::accumulate(hist.begin() + low, hist.begin() + threBin + 1, 0_usize);
    if (totalWeight == 0) {
      cent1 = (threBin + low) / 2.0;
    } else {
      cent1 = totalPos / totalWeight;
    }

    totalPos = std::accumulate(weightPos.begin() + threBin + 1, weightPos.begin() + high + 1, .0);
    totalWeight = std::accumulate(hist.begin() + threBin + 1, hist.begin() + high + 1, 0_usize);
    if (totalWeight == 0) {
      cent2 = (high + threBin) / 2.0;
    } else {
      cent2 = totalPos / totalWeight;
    }

    threBin = (cent1 + cent2) / 2;
  } while (threBin != prevThreBin);

  std::pair<double, double> cent1Range = img.binRange(std::floor(cent1), hist.size());
  cent1 = cent1Range.first + (cent1Range.second - cent1Range.first) * (cent1 - std::floor(cent1));
  std::pair<double, double> cent2Range = img.binRange(std::floor(cent2), hist.size());
  cent2 = cent2Range.first + (cent2Range.second - cent2Range.first) * (cent2 - std::floor(cent2));
  this->reportProgress(1.0);
  return saturate_cast<TVoxel>(img.binRange(threBin, hist.size()).first);
}

template<bool ReportProgress>
template<typename TVoxel>
TVoxel ZImgAutoThreshold<ReportProgress>::typedMaxHistThre(const ZImg& imgIn, size_t c, size_t t)
{
  if (!imgIn.isType<TVoxel>()) {
    throw ZImgException("input img voxel type doesnot match provided type");
  }

  ZImg img = imgIn.createView(c, t);
  std::vector<size_t> hist = img.histogram();

  this->reportProgress(.8);

  size_t low, high;
  histNonZeroRange(hist, low, high);

  if (low + 2 > high) { // two few elements, return smallest possible value as threshold
    this->reportProgress(1.0);
    return img.dataRangeMin<TVoxel>();
  }

  size_t length = high - low + 1;

  size_t searchLimit = length / 2;  // will be at lease 1
  size_t maxBinIdx = std::max_element(hist.begin() + low, hist.begin() + low + searchLimit) - hist.begin();
  this->reportProgress(1.0);
  return saturate_cast<TVoxel>(img.binRange(maxBinIdx + 1, hist.size()).first);
}

// assume hist is not empty
template<bool ReportProgress>
void ZImgAutoThreshold<ReportProgress>::histNonZeroRange(std::vector<size_t>& hist, size_t& low, size_t& high)
{
  low = 0;
  while (low < hist.size() - 1 && hist[low] == 0)
    low++;

  high = hist.size() - 1;
  while (high > low && hist[high] == 0)
    high--;
}

template
class ZImgAutoThreshold<true>;

template
class ZImgAutoThreshold<false>;

template uint8_t ZImgAutoThreshold<true>::typedTriangleThre<uint8_t>(const ZImg&, size_t, size_t);

template uint16_t ZImgAutoThreshold<true>::typedTriangleThre<uint16_t>(const ZImg&, size_t, size_t);

template uint32_t ZImgAutoThreshold<true>::typedTriangleThre<uint32_t>(const ZImg&, size_t, size_t);

template uint64_t ZImgAutoThreshold<true>::typedTriangleThre<uint64_t>(const ZImg&, size_t, size_t);

template int8_t ZImgAutoThreshold<true>::typedTriangleThre<int8_t>(const ZImg&, size_t, size_t);

template int16_t ZImgAutoThreshold<true>::typedTriangleThre<int16_t>(const ZImg&, size_t, size_t);

template int32_t ZImgAutoThreshold<true>::typedTriangleThre<int32_t>(const ZImg&, size_t, size_t);

template int64_t ZImgAutoThreshold<true>::typedTriangleThre<int64_t>(const ZImg&, size_t, size_t);

template float ZImgAutoThreshold<true>::typedTriangleThre<float>(const ZImg&, size_t, size_t);

template double ZImgAutoThreshold<true>::typedTriangleThre<double>(const ZImg&, size_t, size_t);

template uint8_t ZImgAutoThreshold<false>::typedTriangleThre<uint8_t>(const ZImg&, size_t, size_t);

template uint16_t ZImgAutoThreshold<false>::typedTriangleThre<uint16_t>(const ZImg&, size_t, size_t);

template uint32_t ZImgAutoThreshold<false>::typedTriangleThre<uint32_t>(const ZImg&, size_t, size_t);

template uint64_t ZImgAutoThreshold<false>::typedTriangleThre<uint64_t>(const ZImg&, size_t, size_t);

template int8_t ZImgAutoThreshold<false>::typedTriangleThre<int8_t>(const ZImg&, size_t, size_t);

template int16_t ZImgAutoThreshold<false>::typedTriangleThre<int16_t>(const ZImg&, size_t, size_t);

template int32_t ZImgAutoThreshold<false>::typedTriangleThre<int32_t>(const ZImg&, size_t, size_t);

template int64_t ZImgAutoThreshold<false>::typedTriangleThre<int64_t>(const ZImg&, size_t, size_t);

template float ZImgAutoThreshold<false>::typedTriangleThre<float>(const ZImg&, size_t, size_t);

template double ZImgAutoThreshold<false>::typedTriangleThre<double>(const ZImg&, size_t, size_t);

template uint8_t ZImgAutoThreshold<true>::typedCentroidThre<uint8_t>(double&, double&, const ZImg&, size_t, size_t);

template uint16_t ZImgAutoThreshold<true>::typedCentroidThre<uint16_t>(double&, double&, const ZImg&, size_t, size_t);

template uint32_t ZImgAutoThreshold<true>::typedCentroidThre<uint32_t>(double&, double&, const ZImg&, size_t, size_t);

template uint64_t ZImgAutoThreshold<true>::typedCentroidThre<uint64_t>(double&, double&, const ZImg&, size_t, size_t);

template int8_t ZImgAutoThreshold<true>::typedCentroidThre<int8_t>(double&, double&, const ZImg&, size_t, size_t);

template int16_t ZImgAutoThreshold<true>::typedCentroidThre<int16_t>(double&, double&, const ZImg&, size_t, size_t);

template int32_t ZImgAutoThreshold<true>::typedCentroidThre<int32_t>(double&, double&, const ZImg&, size_t, size_t);

template int64_t ZImgAutoThreshold<true>::typedCentroidThre<int64_t>(double&, double&, const ZImg&, size_t, size_t);

template float ZImgAutoThreshold<true>::typedCentroidThre<float>(double&, double&, const ZImg&, size_t, size_t);

template double ZImgAutoThreshold<true>::typedCentroidThre<double>(double&, double&, const ZImg&, size_t, size_t);

template uint8_t ZImgAutoThreshold<false>::typedCentroidThre<uint8_t>(double&, double&, const ZImg&, size_t, size_t);

template uint16_t ZImgAutoThreshold<false>::typedCentroidThre<uint16_t>(double&, double&, const ZImg&, size_t, size_t);

template uint32_t ZImgAutoThreshold<false>::typedCentroidThre<uint32_t>(double&, double&, const ZImg&, size_t, size_t);

template uint64_t ZImgAutoThreshold<false>::typedCentroidThre<uint64_t>(double&, double&, const ZImg&, size_t, size_t);

template int8_t ZImgAutoThreshold<false>::typedCentroidThre<int8_t>(double&, double&, const ZImg&, size_t, size_t);

template int16_t ZImgAutoThreshold<false>::typedCentroidThre<int16_t>(double&, double&, const ZImg&, size_t, size_t);

template int32_t ZImgAutoThreshold<false>::typedCentroidThre<int32_t>(double&, double&, const ZImg&, size_t, size_t);

template int64_t ZImgAutoThreshold<false>::typedCentroidThre<int64_t>(double&, double&, const ZImg&, size_t, size_t);

template float ZImgAutoThreshold<false>::typedCentroidThre<float>(double&, double&, const ZImg&, size_t, size_t);

template double ZImgAutoThreshold<false>::typedCentroidThre<double>(double&, double&, const ZImg&, size_t, size_t);

template uint8_t ZImgAutoThreshold<true>::typedMaxHistThre<uint8_t>(const ZImg&, size_t, size_t);

template uint16_t ZImgAutoThreshold<true>::typedMaxHistThre<uint16_t>(const ZImg&, size_t, size_t);

template uint32_t ZImgAutoThreshold<true>::typedMaxHistThre<uint32_t>(const ZImg&, size_t, size_t);

template uint64_t ZImgAutoThreshold<true>::typedMaxHistThre<uint64_t>(const ZImg&, size_t, size_t);

template int8_t ZImgAutoThreshold<true>::typedMaxHistThre<int8_t>(const ZImg&, size_t, size_t);

template int16_t ZImgAutoThreshold<true>::typedMaxHistThre<int16_t>(const ZImg&, size_t, size_t);

template int32_t ZImgAutoThreshold<true>::typedMaxHistThre<int32_t>(const ZImg&, size_t, size_t);

template int64_t ZImgAutoThreshold<true>::typedMaxHistThre<int64_t>(const ZImg&, size_t, size_t);

template float ZImgAutoThreshold<true>::typedMaxHistThre<float>(const ZImg&, size_t, size_t);

template double ZImgAutoThreshold<true>::typedMaxHistThre<double>(const ZImg&, size_t, size_t);

template uint8_t ZImgAutoThreshold<false>::typedMaxHistThre<uint8_t>(const ZImg&, size_t, size_t);

template uint16_t ZImgAutoThreshold<false>::typedMaxHistThre<uint16_t>(const ZImg&, size_t, size_t);

template uint32_t ZImgAutoThreshold<false>::typedMaxHistThre<uint32_t>(const ZImg&, size_t, size_t);

template uint64_t ZImgAutoThreshold<false>::typedMaxHistThre<uint64_t>(const ZImg&, size_t, size_t);

template int8_t ZImgAutoThreshold<false>::typedMaxHistThre<int8_t>(const ZImg&, size_t, size_t);

template int16_t ZImgAutoThreshold<false>::typedMaxHistThre<int16_t>(const ZImg&, size_t, size_t);

template int32_t ZImgAutoThreshold<false>::typedMaxHistThre<int32_t>(const ZImg&, size_t, size_t);

template int64_t ZImgAutoThreshold<false>::typedMaxHistThre<int64_t>(const ZImg&, size_t, size_t);

template float ZImgAutoThreshold<false>::typedMaxHistThre<float>(const ZImg&, size_t, size_t);

template double ZImgAutoThreshold<false>::typedMaxHistThre<double>(const ZImg&, size_t, size_t);

} // namespace nim
