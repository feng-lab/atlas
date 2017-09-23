#pragma once

#include "zimg.h"
#include "zsaturateoperation.h"

namespace nim {

template<typename TRange>
std::vector<size_t> ZImg::histogram(TRange minData, TRange maxData, size_t nbins, const ZImg& mask) const
{
  if (nbins == 0) {
    nbins = bytesPerVoxel() > 1 ? 65536 : 256;
  }

  std::vector<size_t> res(nbins, 0);

  if (mask.isEmpty()) {
    IMG_TYPED_CALL(histogram_Impl, (*this), res, minData, maxData);
  } else if (isSameSize(mask)) {
    IMG_TYPED_CALL_2TYPE(histogramMask_Impl, (*this), mask, res, minData, maxData, mask);
  } else {
    throw ZImgException(QString("histogram mask has different size <%1> than current img <%2>")
                          .arg(mask.info().toQString()).arg(m_info.toQString()));
  }

  return res;
}

template<typename TRange>
std::pair<double, double> ZImg::binRange(size_t binIdx, TRange minData, TRange maxData, size_t nbins) const
{
  if (nbins == 0) {
    nbins = bytesPerVoxel() > 1 ? 65536 : 256;
  }
  double min;
  double max;
  if (voxelFormat() == VoxelFormat::Float) {
    double minD = minData;
    double maxD = maxData;
    double binSize = (maxD - minD) / nbins;
    min = binIdx * binSize + minD;
    max = min + binSize;
  } else {
    double minD = minData;
    double maxD = maxData;
    double binSize = (maxD - minD + 1) / nbins;
    min = binSize * binIdx + minD;
    max = min + binSize;
  }
  return std::make_pair(min, max);
}

template<typename TPadValue>
ZImg ZImg::cropWithPad(const ZVoxelCoordinate& startCoord, const ZVoxelCoordinate& endCoord,
                       PadOption padOption, TPadValue padValue) const
{
  ZImg res;

  if (isEmpty()) {
    throw ZImgException(QString("Can not crop empty img <%1>").arg(m_info.toQString()));
  }
  if (endCoord.anyLessThan(startCoord)) {
    throw ZImgException(QString("Try to crop pad img with invalid region <%1> to <%2>")
                          .arg(startCoord.toQString()).arg(endCoord.toQString()));
  }
  if (endCoord.anyEqual(startCoord)) {
    return res;
  }

  if (startCoord.allGreaterEqual(0) &&
      endCoord.allLessEqual(ZVoxelCoordinate(m_info.width, m_info.height, m_info.depth,
                                             m_info.numChannels, m_info.numTimes))) {
    res = crop(ZImgRegion(startCoord, endCoord));
  }

  ZImgInfo info = m_info;
  info.width = endCoord.x - startCoord.x;
  info.height = endCoord.y - startCoord.y;
  info.depth = endCoord.z - startCoord.z;
  info.numChannels = endCoord.c - startCoord.c;
  info.numTimes = endCoord.t - startCoord.t;
  info.createDefaultDescriptions();

  res = ZImg(info);

  IMG_TYPED_CALL(cropWithPad_Impl, res,
                 res, startCoord, endCoord,
                 padOption, padValue);

  return res;
}

template<typename TFillValue>
ZImg& ZImg::fill(TFillValue value)
{
  if (bytesPerVoxel() == 1 || value == TFillValue(0)) {
    for (size_t t = 0; t < m_info.numTimes; ++t)
      std::memset(timeData(t), static_cast<unsigned char>(value), timeByteNumber());
    return *this;
  }

  IMG_TYPED_CALL(fill_Impl, (*this), value);
  return *this;
}

template<typename TRange>
ZImg ZImg::normalized(TRange minData, TRange maxData) const
{
  ZImg res(*this);
  res.normalize(minData, maxData);
  return res;
}

template<typename TRange>
ZImg& ZImg::normalize(TRange minData, TRange maxData)
{
  if (isEmpty())
    return *this;
  size_t bytesPerVoxel = m_info.bytesPerVoxel;
  VoxelFormat vf = m_info.voxelFormat;
  if (vf == VoxelFormat::Float) {
    switch (bytesPerVoxel) {
      case 4:
        scale_Impl<float, float>(minData, maxData, this, this);
        break;
      case 8:
        scale_Impl<double, double>(minData, maxData, this, this);
        break;
      default:
        break;
    }
  } else if (vf == VoxelFormat::Signed) {
    switch (bytesPerVoxel) {
      case 1:
        scale_Impl<int8_t, int8_t>(minData, maxData, this, this);
        break;
      case 2:
        scale_Impl<int16_t, int16_t>(minData, maxData, this, this);
        break;
      case 4:
        scale_Impl<int32_t, int32_t>(minData, maxData, this, this);
        break;
      case 8:
        scale_Impl<int64_t, int64_t>(minData, maxData, this, this);
        break;
      default:
        break;
    }
  } else {
    switch (bytesPerVoxel) {
      case 1:
        scale_Impl<uint8_t, uint8_t>(minData, maxData, this, this);
        break;
      case 2:
        scale_Impl<uint16_t, uint16_t>(minData, maxData, this, this);
        break;
      case 4:
        scale_Impl<uint32_t, uint32_t>(minData, maxData, this, this);
        break;
      case 8:
        scale_Impl<uint64_t, uint64_t>(minData, maxData, this, this);
        break;
      default:
        break;
    }
  }

  m_info.validBitCount = 0;
  return *this;
}

template<typename TDesVoxel>
ZImg ZImg::convertTo() const
{
  ZImgInfo info = m_info;
  info.setVoxelFormat<TDesVoxel>();
  ZImg res(info);

  IMG_TYPED_CALL_FIX2NDTYPE(convert_Impl, (*this), TDesVoxel, false, this, &res);
  return res;
}

template<typename TDesVoxel>
ZImg ZImg::convertNormalizedTo() const
{
  ZImgInfo info = m_info;
  info.setVoxelFormat<TDesVoxel>();
  ZImg res(info);

  IMG_TYPED_CALL_FIX2NDTYPE(convert_Impl, (*this), TDesVoxel, true, this, &res);
  return res;
}

template<typename TDesVoxel, typename TRange>
ZImg ZImg::convertTo(TRange minData, TRange maxData) const
{
  ZImgInfo info = m_info;
  info.setVoxelFormat<TDesVoxel>();
  ZImg res(info);

  IMG_TYPED_CALL_FIX2NDTYPE(scale_Impl, (*this), TDesVoxel, minData, maxData, this, &res);

  return res;
}

template<typename TRange>
ZImg ZImg::convertTo(TRange minData, TRange maxData, const ZImg& targetImgType) const
{
  IMG_RETURN_TYPED_CALL(convertTo, targetImgType, minData, maxData);
  return ZImg();
}

template<typename TValue>
ZImg& ZImg::thresholdAbove(TValue threshold, ThresholdMode threMode, TValue outsideValue)
{
  IMG_TYPED_CALL(thresholdAbove_Impl, (*this), threshold, threMode, outsideValue);
  return *this;
}

template<typename TValue>
ZImg& ZImg::thresholdBelow(TValue threshold, ThresholdMode threMode, TValue outsideValue)
{
  IMG_TYPED_CALL(thresholdBelow_Impl, (*this), threshold, threMode, outsideValue);
  return *this;
}

template<typename TValue>
ZImg ZImg::binarized(TValue threshold, ThresholdMode threMode) const
{
  ZImgInfo info = m_info;
  info.voxelFormat = VoxelFormat::Unsigned;
  info.bytesPerVoxel = 1;
  ZImg res(info);

  IMG_TYPED_CALL(binarized_Impl, (*this), res, threshold, threMode);

  return res;
}

template<typename GenericForegroundPredictor>
ZImg ZImg::binarized(const GenericForegroundPredictor& isForeground) const
{
  ZImgInfo info = m_info;
  info.voxelFormat = VoxelFormat::Unsigned;
  info.bytesPerVoxel = 1;
  ZImg res(info);

  IMG_TYPED_CALL(binarized_Impl, (*this), res, isForeground);

  return res;
}

template<typename TVoxel, typename ForegroundPredictor>
ZImg ZImg::typedBinarized(const ForegroundPredictor& isForeground) const
{
  if (!isType<TVoxel>())
    throw ZImgException("Call typedBinarized with wrong type");

  ZImgInfo info = m_info;
  info.voxelFormat = VoxelFormat::Unsigned;
  info.bytesPerVoxel = 1;
  ZImg res(info);

  binarized_Impl<TVoxel>(res, isForeground);
  return res;
}

template<typename TScalar>
ZImg& ZImg::operator+=(TScalar scalar)
{
  static_assert(std::is_arithmetic<TScalar>::value, "Arithmetic not possible on this type");
  if (scalar != TScalar(0)) {
    IMG_TYPED_CALL(addScalar_Impl, (*this), scalar);
  }
  return *this;
}

template<typename TScalarOrZImg>
ZImg ZImg::operator+(const TScalarOrZImg& scalarOrZImg) const
{
  ZImg res(*this);
  res += scalarOrZImg;
  return res;
}

template<typename TScalar>
ZImg& ZImg::operator-=(TScalar scalar)
{
  static_assert(std::is_arithmetic<TScalar>::value, "Arithmetic not possible on this type");
  if (scalar != TScalar(0)) {
    IMG_TYPED_CALL(subScalar_Impl, (*this), scalar);
  }
  return *this;
}

template<typename TScalarOrZImg>
ZImg ZImg::operator-(const TScalarOrZImg& scalarOrZImg) const
{
  ZImg res(*this);
  res -= scalarOrZImg;
  return res;
}

template<typename TScalar>
ZImg& ZImg::operator*=(TScalar scalar)
{
  static_assert(std::is_arithmetic<TScalar>::value, "Arithmetic not possible on this type");
  if (scalar != TScalar(0)) {
    IMG_TYPED_CALL(mulScalar_Impl, (*this), scalar);
  } else {
    fill(0);
  }
  return *this;
}

template<typename TScalarOrZImg>
ZImg ZImg::operator*(const TScalarOrZImg& scalarOrZImg) const
{
  ZImg res(*this);
  res *= scalarOrZImg;
  return res;
}

template<typename TScalar>
ZImg& ZImg::operator/=(TScalar scalar)
{
  static_assert(std::is_arithmetic<TScalar>::value, "Arithmetic not possible on this type");
  if (scalar != TScalar(0)) {
    IMG_TYPED_CALL(divScalar_Impl, (*this), scalar);
  } else {
    throw ZImgException("Can not divide img by zero");
  }
  return *this;
}

template<typename TScalarOrZImg>
ZImg ZImg::operator/(const TScalarOrZImg& scalarOrZImg) const
{
  ZImg res(*this);
  res /= scalarOrZImg;
  return res;
}

template<typename GenericCustomUnaryOp>
ZImg& ZImg::unaryOperation(const GenericCustomUnaryOp& op)
{
  IMG_TYPED_CALL(unaryOp_Impl, (*this), op);
  return *this;
}

template<typename TVoxel, typename CustomUnaryOp>
ZImg& ZImg::typedUnaryOperation(const CustomUnaryOp& op)
{
  if (!isType<TVoxel>())
    throw ZImgException("Call typedUnaryOperation with wrong type");
  unaryOp_Impl<TVoxel>(op);
  return *this;
}

template<typename GenericCustomBinaryOp>
ZImg& ZImg::binaryOperation(const ZImg& other, const GenericCustomBinaryOp& op)
{
  if (!isSameSize(other)) {
    throw ZImgException(QString("img binary operation requires same size img as input: this <%1>, other <%2>")
                          .arg(m_info.toQString()).arg(other.info().toQString()));
  }
  IMG_TYPED_CALL_2TYPE(binaryOp_Impl, (*this), other, other, op);
  return *this;
}

template<typename TVoxel, typename TVoxelOther, typename CustomBinaryOp>
ZImg& ZImg::typedBinaryOperation(const ZImg& other, const CustomBinaryOp& op)
{
  if (!isType<TVoxel>() || !other.isType<TVoxelOther>())
    throw ZImgException("Call typedBinaryOperation with wrong type");
  if (!isSameSize(other)) {
    throw ZImgException(QString("img binary operation requires same size img as input: this <%1>, other <%2>")
                          .arg(m_info.toQString()).arg(other.info().toQString()));
  }
  binaryOp_Impl<TVoxel, TVoxelOther>(other, op);
  return *this;
}

template<typename TValue>
ZVoxelCoordinate ZImg::firstMaxValueCoord(TValue& max, const ZImgRegion& region) const
{
  ZVoxelCoordinate res;
  ZImgRegion rgn = region;
  if (rgn.isEmpty() || !rgn.isValid(m_info)) {
    throw ZImgException(QString("Try to find max value location of img <%1> within invalid region <%2>")
                          .arg(m_info.toQString()).arg(rgn.toQString()));
  }
  rgn.resolveRegionEnd(m_info);
  IMG_TYPED_CALL(firstMaxValueCoord_Impl, (*this), res, max, rgn);
  return res;
}

// coord of all voxels with max img value
template<typename TValue>
std::vector<ZVoxelCoordinate> ZImg::maxValueCoords(TValue& max, const ZImgRegion& region) const
{
  std::vector<ZVoxelCoordinate> res;
  ZImgRegion rgn = region;
  if (rgn.isEmpty() || !rgn.isValid(m_info)) {
    throw ZImgException(QString("Try to find max value locations of img <%1> within invalid region <%2>")
                          .arg(m_info.toQString()).arg(rgn.toQString()));
  }
  rgn.resolveRegionEnd(m_info);
  IMG_TYPED_CALL(maxValueCoords_Impl, (*this), res, max, rgn);
  return res;
}

template<typename TValue>
TValue ZImg::value(const ZVoxelCoordinate& coord) const
{
  if (isEmpty()) {
    throw ZImgException(QString("Can not get voxel value of empty img <%1>").arg(m_info.toQString()));
  }
  if (isCoordValid(coord)) {
    IMG_RETURN_TYPED_CALL(value_Impl, (*this), coord);
    return 0;
  } else {
    throw ZImgException(QString("value: Invalid coordinate %1 of img <%2>")
                          .arg(coord.toQString()).arg(m_info.toQString()));
  }
}

template<typename TValue>
TValue ZImg::value(size_t x, size_t y, size_t z, size_t c, size_t t) const
{
  if (isEmpty()) {
    throw ZImgException(QString("Can not get voxel value of empty img <%1>").arg(m_info.toQString()));
  }
  if (x < m_info.width && y < m_info.height && z < m_info.depth &&
      c < m_info.numChannels && t < m_info.numTimes) {
    IMG_RETURN_TYPED_CALL(value_Impl, (*this), x, y, z, c, t);
    return 0;
  } else {
    throw ZImgException(QString("value: Invalid coordinate (%1,%2,%3,%4,%5) of img <%6>")
                          .arg(x).arg(y).arg(z).arg(c).arg(t).arg(m_info.toQString()));
  }
}

template<typename TValue>
TValue ZImg::value(size_t idx) const
{
  if (isEmpty()) {
    throw ZImgException(QString("Can not get voxel value of empty img <%1>").arg(m_info.toQString()));
  }
  if (idx < voxelNumber()) {
    IMG_RETURN_TYPED_CALL(value_Impl, (*this), idx);
    return 0;
  } else {
    throw ZImgException(QString("value: Invalid voxel idx %1 of img <%2>")
                          .arg(idx).arg(m_info.toQString()));
  }
}

template<typename TValue>
TValue ZImg::valueWithPad(const ZVoxelCoordinate& coord, PadOption padOption, TValue padValue) const
{
  if (isEmpty())
    return 0;
  IMG_RETURN_TYPED_CALL(valueWithPad_Impl, (*this), coord, padOption, padValue);
  return 0;
}

template<typename TValue>
void ZImg::setValue(TValue value, const ZVoxelCoordinate& coord)
{
  if (isEmpty()) {
    throw ZImgException(QString("Can not set voxel value to empty img <%1>").arg(m_info.toQString()));
  }
  if (isCoordValid(coord)) {
    IMG_TYPED_CALL(setValue_Impl, (*this), value, coord);
  } else {
    throw ZImgException(QString("setValue: Invalid coordinate %1 of img <%2>")
                          .arg(coord.toQString()).arg(m_info.toQString()));
  }
}

template<typename TValue>
void ZImg::setValue(TValue value, size_t x, size_t y, size_t z, size_t c, size_t t)
{
  if (isEmpty()) {
    throw ZImgException(QString("Can not set voxel value to empty img <%1>").arg(m_info.toQString()));
  }
  if (x < m_info.width && y < m_info.height && z < m_info.depth &&
      c < m_info.numChannels && t < m_info.numTimes) {
    IMG_TYPED_CALL(setValue_Impl, (*this), value, x, y, z, c, t);
  } else {
    throw ZImgException(QString("setValue: Invalid coordinate (%1,%2,%3,%4,%5) of img <%6>")
                          .arg(x).arg(y).arg(z).arg(c).arg(t).arg(m_info.toQString()));
  }
}

template<typename TValue>
void ZImg::setValue(TValue value, size_t idx)
{
  if (isEmpty()) {
    throw ZImgException(QString("Can not set voxel value to empty img <%1>").arg(m_info.toQString()));
  }
  if (idx < voxelNumber()) {
    IMG_TYPED_CALL(setValue_Impl, (*this), value, idx);
  } else {
    throw ZImgException(QString("value: Invalid voxel idx %1 of img <%2>")
                          .arg(idx).arg(m_info.toQString()));
  }
}

template<typename TValue>
bool ZImg::setValueNoThrow(TValue value, const ZVoxelCoordinate& coord)
{
  if (isCoordValid(coord)) {
    IMG_TYPED_CALL(setValue_Impl, (*this), value, coord);
    return true;
  }
  return false;
}

template<typename TValue>
bool ZImg::setValueNoThrow(TValue value, size_t x, size_t y, size_t z, size_t c, size_t t)
{
  if (!isEmpty() && x < m_info.width && y < m_info.height && z < m_info.depth &&
      c < m_info.numChannels && t < m_info.numTimes) {
    IMG_TYPED_CALL(setValue_Impl, (*this), value, x, y, z, c, t);
    return true;
  }
  return false;
}

template<typename TValue>
bool ZImg::setValueNoThrow(TValue value, size_t idx)
{
  if (!isEmpty() && idx < voxelNumber()) {
    IMG_TYPED_CALL(setValue_Impl, (*this), value, idx);
    return true;
  }
  return false;
}

template<typename TVoxel, typename TDesVoxel>
void ZImg::convert_Impl(bool normalize, const ZImg* src, ZImg* des)
{
  TVoxel minv;
  TVoxel maxv;
  if (normalize) {
    src->computeMinMax(minv, maxv);
  } else {
    minv = src->dataRangeMin<TVoxel>();
    maxv = src->dataRangeMax<TVoxel>();
  }
  //LOG(INFO) << minv << " " << maxv;
  scale_Impl<TVoxel, TDesVoxel>(minv, maxv, src, des);
}

template<typename TVoxel, typename TDesVoxel>
void ZImg::scale_Impl(TVoxel minData, TVoxel maxData, const ZImg* src, ZImg* des)
{
  TDesVoxel dataRangeMin = std::numeric_limits<TDesVoxel>::min();
  TDesVoxel dataRangeMax = std::numeric_limits<TDesVoxel>::max();
  if (des->voxelFormat() == VoxelFormat::Float) {
    dataRangeMin = TDesVoxel(0.0);
    dataRangeMax = TDesVoxel(1.0);
  }

  if (minData == maxData) {
    if (src->voxelFormat() == VoxelFormat::Float) {
      minData = TVoxel(0.0);
      maxData = TVoxel(1.0);
    } else {
      minData = std::numeric_limits<TVoxel>::min();
      maxData = std::numeric_limits<TVoxel>::max();
    }
  }

  if (src->voxelFormat() != VoxelFormat::Float && std::is_same<TVoxel, TDesVoxel>::value &&
      dataRangeMin == TDesVoxel(minData) && dataRangeMax == TDesVoxel(maxData)) {
    if (src != des) {
      for (size_t t = 0; t < src->numTimes(); ++t) {
        std::memcpy(des->timeData(t), src->timeData(t), src->timeByteNumber());
      }
    }
    return;
  }

  // use colormap
  if (src->bytesPerVoxel() <= 2) {  // can not be float
    std::vector<TDesVoxel> colormap;
    buildScaleColormap(minData, maxData, dataRangeMin, dataRangeMax, colormap);
    TVoxel colormapMin = std::numeric_limits<TVoxel>::min();

    for (size_t t = 0; t < src->numTimes(); ++t) {
      for (size_t c = 0; c < src->numChannels(); ++c) {
        const TVoxel* data = src->channelData<TVoxel>(c, t);
        TDesVoxel* desData = des->channelData<TDesVoxel>(c, t);
        for (size_t v = 0; v < src->channelVoxelNumber(); ++v) {
          desData[v] = colormap[data[v] - colormapMin];
        }
      }
    }

  } else {
    for (size_t t = 0; t < src->numTimes(); ++t) {
      for (size_t c = 0; c < src->numChannels(); ++c) {
        const TVoxel* data = src->channelData<TVoxel>(c, t);
        TDesVoxel* desData = des->channelData<TDesVoxel>(c, t);
        for (size_t v = 0; v < src->channelVoxelNumber(); ++v) {
          if (data[v] <= minData)
            desData[v] = dataRangeMin;
          else if (data[v] >= maxData)
            desData[v] = dataRangeMax;
          else {
            desData[v] = (data[v] - minData) * 1.0 / (maxData - minData) * dataRangeMax + dataRangeMin;
          }
        }
      }
    }
  }
}

template<typename TVoxel, typename TDesVoxel>
void ZImg::buildScaleColormap(TVoxel minData, TVoxel maxData, TDesVoxel desDataRangeMin, TDesVoxel desDataRangeMax,
                              std::vector<TDesVoxel>& res)
{
  TVoxel dataRangeMin = std::numeric_limits<TVoxel>::min();
  TVoxel dataRangeMax = std::numeric_limits<TVoxel>::max();
  res.resize(dataRangeMax - dataRangeMin + 1);
  for (TVoxel v = dataRangeMin; v < dataRangeMax; ++v) {
    if (v <= minData)
      res[v - dataRangeMin] = desDataRangeMin;
    else if (v >= maxData)
      res[v - dataRangeMin] = desDataRangeMax;
    else
      res[v - dataRangeMin] = (v - minData) * 1.0 / (maxData - minData) * desDataRangeMax + desDataRangeMin;
  }
  res[dataRangeMax - dataRangeMin] = desDataRangeMax;
}

template<typename TVoxel, typename TScalar>
void ZImg::addScalar_Impl(TScalar scalar)
{
  for (size_t t = 0; t < numTimes(); ++t) {
    TVoxel* data = timeData<TVoxel>(t);
    saturate_add(data, scalar, timeVoxelNumber(), data);
  }
}

template<typename TVoxel, typename TScalar>
void ZImg::subScalar_Impl(TScalar scalar)
{
  for (size_t t = 0; t < numTimes(); ++t) {
    TVoxel* data = timeData<TVoxel>(t);
    saturate_sub(data, scalar, timeVoxelNumber(), data);
  }
}

template<typename TVoxel, typename TScalar>
void ZImg::mulScalar_Impl(TScalar scalar)
{
  for (size_t t = 0; t < numTimes(); ++t) {
    TVoxel* data = timeData<TVoxel>(t);
    saturate_mul(data, scalar, timeVoxelNumber(), data);
  }
}

template<typename TVoxel, typename TScalar>
void ZImg::divScalar_Impl(TScalar scalar)
{
  for (size_t t = 0; t < numTimes(); ++t) {
    TVoxel* data = timeData<TVoxel>(t);
    saturate_div(data, scalar, timeVoxelNumber(), data);
  }
}

template<typename TVoxel, typename GenericCustomUnaryOp>
void ZImg::unaryOp_Impl(const GenericCustomUnaryOp& op)
{
  for (size_t t = 0; t < numTimes(); ++t) {
    TVoxel* data = timeData<TVoxel>(t);
    for (size_t v = 0; v < timeVoxelNumber(); ++v) {
      data[v] = op(data[v]);
    }
  }
}

template<typename TVoxel, typename TVoxelOther, typename GenericCustomBinaryOp>
void ZImg::binaryOp_Impl(const ZImg& other, const GenericCustomBinaryOp& op)
{
  for (size_t t = 0; t < numTimes(); ++t) {
    TVoxel* data = timeData<TVoxel>(t);
    const TVoxelOther* rhsData = other.timeData<TVoxelOther>(t);
    for (size_t v = 0; v < timeVoxelNumber(); ++v) {
      data[v] = op(data[v], rhsData[v]);
    }
  }
}

template<typename TVoxel, typename TValue>
void ZImg::firstMaxValueCoord_Impl(ZVoxelCoordinate& res, TValue& max, const ZImgRegion& region) const
{
  TVoxel maxValue = std::numeric_limits<TVoxel>::min();
  if (voxelFormat() == VoxelFormat::Float)
    maxValue = std::numeric_limits<TVoxel>::lowest();
  if (region.containsWholeTime(m_info)) {
    for (size_t t = region.start.t; t < static_cast<size_t>(region.end.t); ++t) {
      const TVoxel* data = timeData<TVoxel>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        if (data[v] > maxValue) {
          maxValue = data[v];
          res = indexToCoord(v);
          res.t = t;
        }
      }
    }

  } else {
    for (size_t t = region.start.t; t < static_cast<size_t>(region.end.t); ++t) {
      for (size_t c = region.start.c; c < static_cast<size_t>(region.end.c); ++c) {
        for (size_t z = region.start.z; z < static_cast<size_t>(region.end.z); ++z) {
          for (size_t y = region.start.y; y < static_cast<size_t>(region.end.y); ++y) {
            for (size_t x = region.start.x; x < static_cast<size_t>(region.end.x); ++x) {
              TVoxel dat = *(data<TVoxel>(x, y, z, c, t));
              if (dat > maxValue) {
                maxValue = dat;
                res = ZVoxelCoordinate(x, y, z, c, t);
              }
            }
          }
        }
      }
    }

  }
  max = static_cast<TValue>(maxValue);
}

// coord of all voxels with max img value
template<typename TVoxel, typename TValue>
void ZImg::maxValueCoords_Impl(std::vector<ZVoxelCoordinate>& res, TValue& max, const ZImgRegion& region) const
{
  TVoxel maxValue = std::numeric_limits<TVoxel>::min();
  if (voxelFormat() == VoxelFormat::Float)
    maxValue = std::numeric_limits<TVoxel>::lowest();
  if (region.containsWholeTime(m_info)) {
    for (size_t t = region.start.t; t < static_cast<size_t>(region.end.t); ++t) {
      const TVoxel* data = timeData<TVoxel>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        if (data[v] > maxValue) {
          maxValue = data[v];
          res.clear();
          ZVoxelCoordinate coord = indexToCoord(v);
          coord.t = t;
          res.push_back(coord);
        } else if (data[v] == maxValue) {
          ZVoxelCoordinate coord = indexToCoord(v);
          coord.t = t;
          res.push_back(coord);
        }
      }
    }
  } else {
    for (size_t t = region.start.t; t < static_cast<size_t>(region.end.t); ++t) {
      for (size_t c = region.start.c; c < static_cast<size_t>(region.end.c); ++c) {
        for (size_t z = region.start.z; z < static_cast<size_t>(region.end.z); ++z) {
          for (size_t y = region.start.y; y < static_cast<size_t>(region.end.y); ++y) {
            for (size_t x = region.start.x; x < static_cast<size_t>(region.end.x); ++x) {
              TVoxel dat = *(data<TVoxel>(x, y, z, c, t));
              if (dat > maxValue) {
                maxValue = dat;
                res.clear();
                res.emplace_back(x, y, z, c, t);
              } else if (dat == maxValue) {
                res.emplace_back(x, y, z, c, t);
              }
            }
          }
        }
      }
    }
  }
  max = static_cast<TValue>(maxValue);
}

template<typename TVoxel, typename GenericForegroundPredictor>
void ZImg::binarized_Impl(ZImg& res, const GenericForegroundPredictor& isForeground) const
{
  for (size_t t = 0; t < numTimes(); ++t) {
    const TVoxel* data = timeData<TVoxel>(t);
    uint8_t* resData = res.timeData<uint8_t>(t);
    for (size_t v = 0; v < timeVoxelNumber(); ++v) {
      if (isForeground(data[v]))
        resData[v] = 1;
    }
  }
}

} // namespace nim
