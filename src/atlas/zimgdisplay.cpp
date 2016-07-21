#include "zimgdisplay.h"
#ifndef _USE_QTCONCURRENT_
#include <tbb/parallel_for.h>
#else
#include <QtConcurrentRun>
#endif
#include "QsLog.h"
#include <functional>
#ifdef _USE_MSVC2013_
#include <boost/bind.hpp>
#endif

namespace nim {

ZImgDisplay::ZImgDisplay(const ZImg &img)
  : m_img(img)
{
  assert(!m_img.isEmpty());
  reset();
}

ZImgDisplay::~ZImgDisplay()
{
}

void ZImgDisplay::reset()
{
  m_z = 0;
  m_t = 0;
  m_alpha = 1.0;

  hideAllChannels();
}

void ZImgDisplay::showChannel(size_t ch, double minData, double maxData)
{
  assert(minData <= maxData);

  if (minData == maxData) {
    if (minData > m_img.dataRangeMin<double>())
      minData = m_img.dataRangeMin<double>();
    else
      maxData = minData + 1.;
  }

  if (ch < m_img.numChannels()) {
    m_channels[ch] = std::make_pair(minData, maxData);
  }
}

void ZImgDisplay::hideChannel(size_t ch)
{
  std::map<size_t, std::pair<double,double>>::iterator it = m_channels.find(ch);
  if (it != m_channels.end())
    m_channels.erase(it);
}

void ZImgDisplay::showAllChannels(double minData, double maxData)
{
  assert(minData <= maxData);

  m_channels.clear();

  if (minData == maxData) {
    if (minData > m_img.dataRangeMin<double>())
      minData = m_img.dataRangeMin<double>();
    else
      maxData = minData + 1.;
  }

  for (size_t i=0; i<m_img.numChannels(); ++i) {
    showChannel(i, minData, maxData);
  }
}

void ZImgDisplay::hideAllChannels()
{
  m_channels.clear();
}

size_t ZImgDisplay::toQImageSizeLimit()
{
  return 8000;
}

QImage ZImgDisplay::toQImage() const
{
  assert(m_img.width() <= toQImageSizeLimit() && m_img.height() <= toQImageSizeLimit());

  QImage res(m_img.width(), m_img.height(), QImage::Format_ARGB32_Premultiplied);
  fillQImage(m_img, res);
  return res;
}

ZQImagePack ZImgDisplay::toQImagePack(size_t tileWidth, size_t tileHeight) const
{
  ZQImagePack resV;
  if (m_channels.empty())
    return resV;

  if (m_img.width() <= tileWidth && m_img.height() <= tileHeight) {
    QImage res(m_img.width(), m_img.height(), QImage::Format_ARGB32_Premultiplied);
    fillQImage(m_img, res);
    resV.addImage(res, QPoint(0,0));
  } else {
    size_t zBackup = m_z;
    size_t tBackup = m_t;
    m_z = 0;
    m_t = 0;
    size_t lastCol = m_img.width() % tileWidth;
    size_t lastRow = m_img.height() % tileHeight;
    size_t numX = m_img.width() / tileWidth + (lastCol > 0);
    size_t numY = m_img.height() / tileHeight + (lastRow > 0);
    for (size_t x=0; x<numX; ++x) {
      for (size_t y=0; y<numY; ++y) {
        size_t startX = x * tileWidth;
        size_t endX = std::min(m_img.width(), startX + tileWidth + 1);
        size_t startY = y * tileHeight;
        size_t endY = std::min(m_img.height(), startY + tileHeight + 1);
        ZImg croped = m_img.crop(ZImgRegion(startX, endX, startY, endY, zBackup, zBackup+1,
                                            0, -1, tBackup, tBackup+1));
        QImage res(croped.width(), croped.height(), QImage::Format_ARGB32_Premultiplied);
        fillQImage(croped, res);
        resV.addImage(res, QPoint(startX,startY));
      }
    }
    m_z = zBackup;
    m_t = tBackup;
  }

  return resV;
}

template<typename TVoxel>
#ifndef _USE_QTCONCURRENT_
void ZImgDisplay::setQImageDataBlockCM(const ZImg *img, QImage *qim, const tbb::blocked_range<size_t> &rowRange,
#else
void ZImgDisplay::setQImageDataBlockCM(const ZImg *img, QImage *qim, std::pair<size_t, size_t> rowRange,
#endif
                                       const std::vector<size_t>* channels,
                                       const std::vector<ZImgVoxelColormap<TVoxel>> *colormaps) const
{
  std::vector<const TVoxel*> imgDatas(channels->size());
#ifndef _USE_QTCONCURRENT_
  for (size_t c=0; c<channels->size(); ++c) {
    imgDatas[c] = img->rowData<TVoxel>(rowRange.begin(), m_z, (*channels)[c], m_t);
  }
  for (size_t i=rowRange.begin(); i != rowRange.end(); ++i) {
#else
  for (size_t c=0; c<channels->size(); ++c) {
    imgDatas[c] = img->rowData<TVoxel>(rowRange.first, m_z, (*channels)[c], m_t);
  }
  for (size_t i=rowRange.first; i<rowRange.second; ++i) {
#endif
    QRgb* qimData = bit_cast<QRgb*>(qim->scanLine(i));

    for (int j=0; j<qim->width(); ++j) {
      col4 col = colormaps->at(0).color(*(imgDatas[0])++);
      for (size_t c=1; c<channels->size(); ++c) {
        col.max(colormaps->at(c).color(*(imgDatas[c])++));
      }

      qimData[j] = qRgba(col.r, col.g, col.b, col.a);
    }
  }
}

template<typename TVoxel>
#ifndef _USE_QTCONCURRENT_
void ZImgDisplay::setQImageDataBlockCMMultAlpha(const ZImg *img, QImage *qim, const tbb::blocked_range<size_t> &rowRange,
#else
void ZImgDisplay::setQImageDataBlockCMMultAlpha(const ZImg *img, QImage *qim, std::pair<size_t, size_t> rowRange,
#endif
                                                const std::vector<size_t>* channels,
                                                const std::vector<ZImgVoxelColormap<TVoxel>> *colormaps) const
{
  std::vector<const TVoxel*> imgDatas(channels->size());
#ifndef _USE_QTCONCURRENT_
  for (size_t c=0; c<channels->size(); ++c) {
    imgDatas[c] = img->rowData<TVoxel>(rowRange.begin(), m_z, (*channels)[c], m_t);
  }
  for (size_t i=rowRange.begin(); i != rowRange.end(); ++i) {
#else
  for (size_t c=0; c<channels->size(); ++c) {
    imgDatas[c] = img->rowData<TVoxel>(rowRange.first, m_z, (*channels)[c], m_t);
  }
  for (size_t i=rowRange.first; i<rowRange.second; ++i) {
#endif
    QRgb* qimData = bit_cast<QRgb*>(qim->scanLine(i));

    for (int j=0; j<qim->width(); ++j) {
      col4 col = colormaps->at(0).color(*(imgDatas[0])++);
      for (size_t c=1; c<channels->size(); ++c) {
        col.max(colormaps->at(c).color(*(imgDatas[c])++));
      }
      float a = col.a / 255.f;
      qimData[j] = qRgba(static_cast<uint8_t>(col.r * a + .5f),
                         static_cast<uint8_t>(col.g * a + .5f),
                         static_cast<uint8_t>(col.b * a + .5f),
                         col.a);
    }
  }
}

template<typename TVoxel>
void ZImgDisplay::setQImageDataCM(const ZImg &img, QImage &qim) const
{
  size_t numChannelsToShow = m_channels.size();
  std::vector<size_t> channels(numChannelsToShow);
  std::vector<ZImgVoxelColormap<TVoxel>> colormaps(numChannelsToShow);
  TVoxel minimum = std::numeric_limits<TVoxel>::min();
  TVoxel maximum = std::numeric_limits<TVoxel>::max();
  size_t idx=0;
  int alphaChannelIdx = -1;
  for (std::map<size_t, std::pair<double,double>>::const_iterator it = m_channels.begin();
       it != m_channels.end(); ++it) {
    if (m_img.info().isAlphaChannel(it->first)) {
      alphaChannelIdx = idx;
      break;
    }
    idx++;
  }
  idx = 0;
  if (alphaChannelIdx < 0) {
    for (std::map<size_t, std::pair<double,double>>::const_iterator it = m_channels.begin();
         it != m_channels.end(); ++it) {
      size_t ch = it->first;
      double minValue = it->second.first;
      double maxValue = it->second.second;
      channels[idx] = ch;
      colormaps[idx].setRange(minimum, maximum);
      col4 maxCol(m_img.channelColor(ch));
      maxCol.a = roundTo<uint8_t>(m_alpha * 255);
      for (TVoxel v = minimum; v < maximum; ++v) {
        double coef = (v - minValue) / (maxValue - minValue);
        colormaps[idx].color(v) = scaleDownColorRGB(maxCol, coef);
      }
      colormaps[idx].color(maximum) = scaleDownColorRGB(maxCol, (maximum - minValue) / (maxValue - minValue));
      idx++;
    }
  } else {
    for (std::map<size_t, std::pair<double,double>>::const_iterator it = m_channels.begin();
         it != m_channels.end(); ++it) {
      size_t ch = it->first;
      double minValue = it->second.first;
      double maxValue = it->second.second;
      channels[idx] = ch;
      colormaps[idx].setRange(minimum, maximum);
      if (m_img.info().isAlphaChannel(ch)) {
        col4 maxCol(0,0,0,roundTo<uint8_t>(m_alpha * 255));
        for (TVoxel v = minimum; v < maximum; ++v) {
          double coef = (v - minValue) / (maxValue - minValue);
          colormaps[idx].color(v) = scaleDownColorRGBA(maxCol, coef);
        }
        colormaps[idx].color(maximum) = scaleDownColorRGBA(maxCol, (maximum - minValue) / (maxValue - minValue));
      } else {
        col4 maxCol(m_img.channelColor(ch));
        maxCol.a = 0;
        for (TVoxel v = minimum; v < maximum; ++v) {
          double coef = (v - minValue) / (maxValue - minValue);
          colormaps[idx].color(v) = scaleDownColorRGBA(maxCol, coef);
        }
        colormaps[idx].color(maximum) = scaleDownColorRGBA(maxCol, (maximum - minValue) / (maxValue - minValue));
      }
      idx++;
    }
  }

  //setQImageDataBlockCM<TVoxel>(qim, 0, m_img.height(), channels, colormaps);
  //return;

#ifndef _USE_QTCONCURRENT_
#ifdef _USE_MSVC2013_
  if (alphaChannelIdx < 0) {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, img.height()),
                      boost::bind(&ZImgDisplay::setQImageDataBlockCM<TVoxel>, this, &img, &qim,
                                  _1, &channels, &colormaps));
  } else {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, img.height()),
                      boost::bind(&ZImgDisplay::setQImageDataBlockCMMultAlpha<TVoxel>, this, &img, &qim,
                                  _1, &channels, &colormaps));
  }
#else
  if (alphaChannelIdx < 0) {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, img.height()),
                      std::bind(&ZImgDisplay::setQImageDataBlockCM<TVoxel>, this, &img, &qim,
                                std::placeholders::_1, &channels, &colormaps));
  } else {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, img.height()),
                      std::bind(&ZImgDisplay::setQImageDataBlockCMMultAlpha<TVoxel>, this, &img, &qim,
                                std::placeholders::_1, &channels, &colormaps));
  }
#endif
#else
  size_t numBlock = std::min(img.height(), size_t(32));
  size_t blockHeight = img.height() / numBlock;
  std::vector<QFuture<void>> res(numBlock);
  for (size_t i=0; i<numBlock; ++i) {
    size_t startLine = i * blockHeight;
    size_t endLine = (i+1) * blockHeight;
    if (i == numBlock - 1)
      endLine = img.height();
    if (alphaChannelIdx < 0) {
      res[i] = QtConcurrent::run(this, &ZImgDisplay::setQImageDataBlockCM<TVoxel>, &img, &qim,
                                 std::make_pair(startLine, endLine), &channels, &colormaps);
    } else {
      res[i] = QtConcurrent::run(this, &ZImgDisplay::setQImageDataBlockCMMultAlpha<TVoxel>, &img, &qim,
                                 std::make_pair(startLine, endLine), &channels, &colormaps);
    }
  }
  for (size_t i=0; i<numBlock; ++i)
    res[i].waitForFinished();
#endif
}

template<typename TVoxel>
#ifndef _USE_QTCONCURRENT_
void ZImgDisplay::setQImageDataBlock(const ZImg *img, QImage *qim, const tbb::blocked_range<size_t> &rowRange,
#else
void ZImgDisplay::setQImageDataBlock(const ZImg *img, QImage *qim, std::pair<size_t, size_t> rowRange,
#endif
                                     const std::vector<size_t> *channels) const
{
  std::vector<const TVoxel*> imgDatas(channels->size());
  std::vector<double> chMinValue(channels->size());
  std::vector<double> chMaxValue(channels->size());
  std::vector<col4> chCol(channels->size());
  int alphaChannelIdx = -1;
  for (size_t c=0; c<channels->size(); ++c) {
    size_t ch = channels->at(c);
#ifndef _USE_QTCONCURRENT_
    imgDatas[c] = img->rowData<TVoxel>(rowRange.begin(), m_z, ch, m_t);
#else
    imgDatas[c] = img->rowData<TVoxel>(rowRange.first, m_z, ch, m_t);
#endif
    chMinValue[c] = m_channels.at(ch).first;
    chMaxValue[c] = m_channels.at(ch).second;
    chCol[c] = m_img.channelColor(ch);
    chCol[c].a = roundTo<uint8_t>(m_alpha * 255);
    if (m_img.info().isAlphaChannel(ch))
        alphaChannelIdx = c;
  }
  if (alphaChannelIdx < 0) {
#ifndef _USE_QTCONCURRENT_
    for (size_t i=rowRange.begin(); i != rowRange.end(); ++i) {
#else
    for (size_t i=rowRange.first; i<rowRange.second; ++i) {
#endif
      QRgb* qimData = bit_cast<QRgb*>(qim->scanLine(i));

      for (int j=0; j<qim->width(); ++j) {
        TVoxel v = *(imgDatas[0])++;
        col4 col = scaleDownColorRGB(chCol[0], (v-chMinValue[0]) / (chMaxValue[0]-chMinValue[0]));
        for (size_t c=1; c<channels->size(); ++c) {
          v = *(imgDatas[c])++;
          col.max(scaleDownColorRGB(chCol[c], (v-chMinValue[c]) / (chMaxValue[c]-chMinValue[c])));
        }

        qimData[j] = qRgba(col.r, col.g, col.b, col.a);
      }
    }
  } else {
    if (channels->size() == 1) {
#ifndef _USE_QTCONCURRENT_
      for (size_t i=rowRange.begin(); i != rowRange.end(); ++i) {
#else
      for (size_t i=rowRange.first; i<rowRange.second; ++i) {
#endif
        QRgb* qimData = bit_cast<QRgb*>(qim->scanLine(i));

        for (int j=0; j<qim->width(); ++j) {
          TVoxel v = *(imgDatas[0])++;
          int a = roundTo<uint8_t>(m_alpha * (v-chMinValue[0]) / (chMaxValue[0]-chMinValue[0]) * 255);
          qimData[j] = qRgba(0, 0, 0, a);
        }
      }
    } else {
      for (size_t c=0; c<channels->size()-1; ++c) {
        chCol[c].a = 255;
      }
#ifndef _USE_QTCONCURRENT_
      for (size_t i=rowRange.begin(); i != rowRange.end(); ++i) {
#else
      for (size_t i=rowRange.first; i<rowRange.second; ++i) {
#endif
        QRgb* qimData = bit_cast<QRgb*>(qim->scanLine(i));

        for (int j=0; j<qim->width(); ++j) {
          size_t c = 0;
          TVoxel v = *(imgDatas[c])++;
          col4 col = scaleDownColorRGB(chCol[c], (v-chMinValue[c]) / (chMaxValue[c]-chMinValue[c]));
          for (c=1; c<channels->size()-1; ++c) {
            v = *(imgDatas[c])++;
            col.max(scaleDownColorRGB(chCol[c], (v-chMinValue[c]) / (chMaxValue[c]-chMinValue[c])));
          }
          v = *(imgDatas[c])++;
          double a = m_alpha * (v-chMinValue[c]) / (chMaxValue[c]-chMinValue[c]);
          if (a < 0) {
            a = 0;
          } else if (a > 1) {
            a = 1;
          }

          qimData[j] = qRgba(roundTo<uint8_t>(col.r * a), roundTo<uint8_t>(col.g * a),
                             roundTo<uint8_t>(col.b * a), roundTo<uint8_t>(a * 255));
        }
      }
    }
  }
}

template<typename TVoxel>
void ZImgDisplay::setQImageData(const ZImg &img, QImage &qim) const
{
  std::vector<size_t> channels(m_channels.size());
  size_t idx=0;
  for (std::map<size_t, std::pair<double,double>>::const_iterator it = m_channels.begin();
       it != m_channels.end(); ++it) {
    channels[idx++] = it->first;
  }

#ifndef _USE_QTCONCURRENT_
#ifdef _USE_MSVC2013_
  tbb::parallel_for(tbb::blocked_range<size_t>(0, img.height()),
                    boost::bind(&ZImgDisplay::setQImageDataBlock<TVoxel>, this, &img, &qim,
                                _1, &channels));
#else
  tbb::parallel_for(tbb::blocked_range<size_t>(0, img.height()),
                    std::bind(&ZImgDisplay::setQImageDataBlock<TVoxel>, this, &img, &qim,
                              std::placeholders::_1, &channels));
#endif
#else
  size_t numBlock = std::min(img.height(), size_t(32));
  size_t blockHeight = img.height() / numBlock;
  std::vector<QFuture<void>> res(numBlock);
  for (size_t i=0; i<numBlock; ++i) {
    size_t startLine = i * blockHeight;
    size_t endLine = (i+1) * blockHeight;
    if (i == numBlock - 1)
      endLine = img.height();
    res[i] = QtConcurrent::run(this, &ZImgDisplay::setQImageDataBlock<TVoxel>, &img, &qim,
                               std::make_pair(startLine, endLine), &channels);
  }
  for (size_t i=0; i<numBlock; ++i)
    res[i].waitForFinished();
#endif
}

void ZImgDisplay::fillQImage(const ZImg &img, QImage &qim) const
{
  if (m_channels.empty())
    return;
  size_t bytesPerVoxel = img.bytesPerVoxel();
  VoxelFormat vf = img.voxelFormat();
  if (vf == VoxelFormat::Float) {
    switch (bytesPerVoxel) {
    case 4:
      setQImageData<float>(img, qim);
      break;
    case 8:
      setQImageData<double>(img, qim);
      break;
    default:
      break;
    }
  } else if (vf == VoxelFormat::Signed) {
    switch (bytesPerVoxel) {
    case 1:
      setQImageDataCM<int8_t>(img, qim);
      break;
    case 2:
      setQImageDataCM<int16_t>(img, qim);
      break;
    case 4:
      setQImageData<int32_t>(img, qim);
      break;
    case 8:
      setQImageData<int64_t>(img, qim);
      break;
    default:
      break;
    }
  } else {
    switch (bytesPerVoxel) {
    case 1:
      setQImageDataCM<uint8_t>(img, qim);
      break;
    case 2:
      setQImageDataCM<uint16_t>(img, qim);
      break;
    case 4:
      setQImageData<uint32_t>(img, qim);
      break;
    case 8:
      setQImageData<uint64_t>(img, qim);
      break;
    default:
      break;
    }
  }
}

} // namespace nim
