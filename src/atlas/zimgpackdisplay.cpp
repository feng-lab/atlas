#include "zimgpackdisplay.h"
#include <tbb/parallel_for.h>
#include "zlog.h"
#include <functional>
#include "zimgdisplay.h"

namespace nim {

ZImgPackDisplay::ZImgPackDisplay(const ZImgPack& imgPack, bool mip)
  : m_imgPack(imgPack), m_mip(mip)
{
  reset();
}

ZImgPackDisplay::~ZImgPackDisplay()
{
}

void ZImgPackDisplay::reset()
{
  m_z = 0;
  m_t = 0;
  m_scale = 1.0;
  m_alpha = 1.0;

  hideAllChannels();
}

void ZImgPackDisplay::showChannel(size_t ch, double minData, double maxData)
{
  CHECK(minData <= maxData);

  if (minData == maxData) {
    if (minData > m_imgPack.rangeMin())
      minData = m_imgPack.rangeMin();
    else
      maxData = minData + 1.;
  }

  if (ch < m_imgPack.imgInfo().numChannels) {
    m_channels[ch] = std::make_pair(minData, maxData);
  }
}

void ZImgPackDisplay::hideChannel(size_t ch)
{
  std::map<size_t, std::pair<double, double>>::iterator it = m_channels.find(ch);
  if (it != m_channels.end())
    m_channels.erase(it);
}

void ZImgPackDisplay::showAllChannels(double minData, double maxData)
{
  CHECK(minData <= maxData);
  m_channels.clear();

  if (minData == maxData) {
    if (minData > m_imgPack.rangeMin())
      minData = m_imgPack.rangeMin();
    else
      maxData = minData + 1.;
  }

  for (size_t i = 0; i < m_imgPack.imgInfo().numChannels; ++i) {
    showChannel(i, minData, maxData);
  }
}

void ZImgPackDisplay::hideAllChannels()
{
  m_channels.clear();
}

ZQImagePack ZImgPackDisplay::toQImagePack(size_t tileWidth, size_t tileHeight) const
{
  CHECK(!m_viewport.isNull() && m_scale > 0);

  if (!m_imgPack.isDiskCached()) {
    ZImgDisplay display(m_mip ? m_imgPack.maxZProjectedImg() : m_imgPack.img());
    display.setSlice(m_z);
    display.setTime(m_t);
    display.setAlpha(m_alpha);
    for (auto it = m_channels.cbegin(); it != m_channels.cend(); ++it) {
      display.showChannel(it->first, it->second.first, it->second.second);
    }
    return display.toQImagePack(tileWidth, tileHeight);

  } else {

    ZQImagePack resV;
    if (m_channels.empty())
      return resV;

    std::vector<std::shared_ptr<ZImg>> imgs;
    std::vector<QPoint> locs;
    std::vector<double> scales;

    m_imgPack.retrieveCoveredImgs(imgs, locs, scales, m_z, m_t, m_viewport, m_scale, m_mip);

    for (size_t i = 0; i < imgs.size(); ++i) {
      if (imgs[i]->width() <= tileWidth && imgs[i]->height() <= tileHeight) {
        QImage res(imgs[i]->width(), imgs[i]->height(), QImage::Format_ARGB32_Premultiplied);
        fillQImage(*imgs[i], res);
        resV.addImage(res, locs[i], scales[i]);
      } else {
        size_t lastCol = imgs[i]->width() % tileWidth;
        size_t lastRow = imgs[i]->height() % tileHeight;
        size_t numX = imgs[i]->width() / tileWidth + (lastCol > 0);
        size_t numY = imgs[i]->height() / tileHeight + (lastRow > 0);
        for (size_t x = 0; x < numX; ++x) {
          for (size_t y = 0; y < numY; ++y) {
            size_t startX = x * tileWidth;
            size_t endX = std::min(imgs[i]->width(), startX + tileWidth + 1);
            size_t startY = y * tileHeight;
            size_t endY = std::min(imgs[i]->height(), startY + tileHeight + 1);
            ZImg croped = imgs[i]->crop(ZImgRegion(startX, endX, startY, endY));
            QImage res(croped.width(), croped.height(), QImage::Format_ARGB32_Premultiplied);
            fillQImage(croped, res);
            resV.addImage(res, locs[i] + QPoint(startX, startY), scales[i]);
          }
        }
      }
    }

    return resV;

  }
}

template<typename TVoxel>
void ZImgPackDisplay::setQImageDataBlockCM(const ZImg* img, QImage* qim, const tbb::blocked_range<size_t>& rowRange,
                                           const std::vector<size_t>* channels,
                                           const std::vector<ZImgVoxelColormap<TVoxel>>* colormaps) const
{
  std::vector<const TVoxel*> imgDatas(channels->size());
  for (size_t c = 0; c < channels->size(); ++c) {
    imgDatas[c] = img->rowData<TVoxel>(rowRange.begin(), 0, (*channels)[c], 0);
  }
  for (size_t i = rowRange.begin(); i != rowRange.end(); ++i) {
    QRgb* qimData = bit_cast<QRgb*>(qim->scanLine(i));

    for (int j = 0; j < qim->width(); ++j) {
      col4 col = colormaps->at(0).color(*(imgDatas[0])++);
      for (size_t c = 1; c < channels->size(); ++c) {
        col.max(colormaps->at(c).color(*(imgDatas[c])++));
      }

      qimData[j] = qRgba(col.r, col.g, col.b, col.a);
    }
  }
}

template<typename TVoxel>
void
ZImgPackDisplay::setQImageDataBlockCMMultAlpha(const ZImg* img, QImage* qim, const tbb::blocked_range<size_t>& rowRange,
                                               const std::vector<size_t>* channels,
                                               const std::vector<ZImgVoxelColormap<TVoxel>>* colormaps) const
{
  std::vector<const TVoxel*> imgDatas(channels->size());
  for (size_t c = 0; c < channels->size(); ++c) {
    imgDatas[c] = img->rowData<TVoxel>(rowRange.begin(), 0, (*channels)[c], 0);
  }
  for (size_t i = rowRange.begin(); i != rowRange.end(); ++i) {
    QRgb* qimData = bit_cast<QRgb*>(qim->scanLine(i));

    for (int j = 0; j < qim->width(); ++j) {
      col4 col = colormaps->at(0).color(*(imgDatas[0])++);
      for (size_t c = 1; c < channels->size(); ++c) {
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
void ZImgPackDisplay::setQImageDataCM(const ZImg& img, QImage& qim) const
{
  size_t numChannelsToShow = m_channels.size();
  std::vector<size_t> channels(numChannelsToShow);
  std::vector<ZImgVoxelColormap<TVoxel>> colormaps(numChannelsToShow);
  TVoxel minimum = std::numeric_limits<TVoxel>::min();
  TVoxel maximum = std::numeric_limits<TVoxel>::max();
  size_t idx = 0;
  int alphaChannelIdx = -1;
  for (std::map<size_t, std::pair<double, double>>::const_iterator it = m_channels.begin();
       it != m_channels.end(); ++it) {
    if (m_imgPack.imgInfo().isAlphaChannel(it->first)) {
      alphaChannelIdx = idx;
      break;
    }
    idx++;
  }
  idx = 0;
  if (alphaChannelIdx < 0) {
    for (std::map<size_t, std::pair<double, double>>::const_iterator it = m_channels.begin();
         it != m_channels.end(); ++it) {
      size_t ch = it->first;
      double minValue = it->second.first;
      double maxValue = it->second.second;
      channels[idx] = ch;
      colormaps[idx].setRange(minimum, maximum);
      col4 maxCol(m_imgPack.imgInfo().channelColors[ch]);
      maxCol.a = roundTo<uint8_t>(m_alpha * 255);
      for (TVoxel v = minimum; v < maximum; ++v) {
        double coef = (v - minValue) / (maxValue - minValue);
        colormaps[idx].color(v) = scaleDownColorRGB(maxCol, coef);
      }
      colormaps[idx].color(maximum) = scaleDownColorRGB(maxCol, (maximum - minValue) / (maxValue - minValue));
      idx++;
    }
  } else {
    for (std::map<size_t, std::pair<double, double>>::const_iterator it = m_channels.begin();
         it != m_channels.end(); ++it) {
      size_t ch = it->first;
      double minValue = it->second.first;
      double maxValue = it->second.second;
      channels[idx] = ch;
      colormaps[idx].setRange(minimum, maximum);
      if (m_imgPack.imgInfo().isAlphaChannel(ch)) {
        col4 maxCol(0, 0, 0, roundTo<uint8_t>(m_alpha * 255));
        for (TVoxel v = minimum; v < maximum; ++v) {
          double coef = (v - minValue) / (maxValue - minValue);
          colormaps[idx].color(v) = scaleDownColorRGBA(maxCol, coef);
        }
        colormaps[idx].color(maximum) = scaleDownColorRGBA(maxCol, (maximum - minValue) / (maxValue - minValue));
      } else {
        col4 maxCol(m_imgPack.imgInfo().channelColors[ch]);
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

  if (alphaChannelIdx < 0) {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, img.height()),
                      [&](const tbb::blocked_range<size_t>& range) {
                        setQImageDataBlockCM<TVoxel>(&img, &qim, range, &channels, &colormaps);
                      });
  } else {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, img.height()),
                      [&](const tbb::blocked_range<size_t>& range) {
                        setQImageDataBlockCMMultAlpha<TVoxel>(&img, &qim, range, &channels, &colormaps);
                      });
  }
}

template<typename TVoxel>
void ZImgPackDisplay::setQImageDataBlock(const ZImg* img, QImage* qim, const tbb::blocked_range<size_t>& rowRange,
                                         const std::vector<size_t>* channels) const
{
  std::vector<const TVoxel*> imgDatas(channels->size());
  std::vector<double> chMinValue(channels->size());
  std::vector<double> chMaxValue(channels->size());
  std::vector<col4> chCol(channels->size());
  int alphaChannelIdx = -1;
  for (size_t c = 0; c < channels->size(); ++c) {
    size_t ch = channels->at(c);
    imgDatas[c] = img->rowData<TVoxel>(rowRange.begin(), 0, ch, 0);
    chMinValue[c] = m_channels.at(ch).first;
    chMaxValue[c] = m_channels.at(ch).second;
    chCol[c] = m_imgPack.imgInfo().channelColors[ch];
    chCol[c].a = roundTo<uint8_t>(m_alpha * 255);
    if (m_imgPack.imgInfo().isAlphaChannel(ch))
      alphaChannelIdx = c;
  }
  if (alphaChannelIdx < 0) {
    for (size_t i = rowRange.begin(); i != rowRange.end(); ++i) {
      QRgb* qimData = bit_cast<QRgb*>(qim->scanLine(i));

      for (int j = 0; j < qim->width(); ++j) {
        TVoxel v = *(imgDatas[0])++;
        col4 col = scaleDownColorRGB(chCol[0], (v - chMinValue[0]) / (chMaxValue[0] - chMinValue[0]));
        for (size_t c = 1; c < channels->size(); ++c) {
          v = *(imgDatas[c])++;
          col.max(scaleDownColorRGB(chCol[c], (v - chMinValue[c]) / (chMaxValue[c] - chMinValue[c])));
        }

        qimData[j] = qRgba(col.r, col.g, col.b, col.a);
      }
    }
  } else {
    if (channels->size() == 1) {
      for (size_t i = rowRange.begin(); i != rowRange.end(); ++i) {
        QRgb* qimData = bit_cast<QRgb*>(qim->scanLine(i));

        for (int j = 0; j < qim->width(); ++j) {
          TVoxel v = *(imgDatas[0])++;
          int a = roundTo<uint8_t>(m_alpha * (v - chMinValue[0]) / (chMaxValue[0] - chMinValue[0]) * 255);
          qimData[j] = qRgba(0, 0, 0, a);
        }
      }
    } else {
      for (size_t c = 0; c < channels->size() - 1; ++c) {
        chCol[c].a = 255;
      }
      for (size_t i = rowRange.begin(); i != rowRange.end(); ++i) {
        QRgb* qimData = bit_cast<QRgb*>(qim->scanLine(i));

        for (int j = 0; j < qim->width(); ++j) {
          size_t c = 0;
          TVoxel v = *(imgDatas[c])++;
          col4 col = scaleDownColorRGB(chCol[c], (v - chMinValue[c]) / (chMaxValue[c] - chMinValue[c]));
          for (c = 1; c < channels->size() - 1; ++c) {
            v = *(imgDatas[c])++;
            col.max(scaleDownColorRGB(chCol[c], (v - chMinValue[c]) / (chMaxValue[c] - chMinValue[c])));
          }
          v = *(imgDatas[c])++;
          double a = m_alpha * (v - chMinValue[c]) / (chMaxValue[c] - chMinValue[c]);
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
void ZImgPackDisplay::setQImageData(const ZImg& img, QImage& qim) const
{
  std::vector<size_t> channels(m_channels.size());
  size_t idx = 0;
  for (std::map<size_t, std::pair<double, double>>::const_iterator it = m_channels.begin();
       it != m_channels.end(); ++it) {
    channels[idx++] = it->first;
  }

  tbb::parallel_for(tbb::blocked_range<size_t>(0, img.height()),
                    [&](const tbb::blocked_range<size_t>& range) {
                      setQImageDataBlock<TVoxel>(&img, &qim, range, &channels);
                    });
}

void ZImgPackDisplay::fillQImage(const ZImg& img, QImage& qim) const
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

