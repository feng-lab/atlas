#include "zimgpackdisplay.h"

#include "zimgdisplay.h"
#include "zlog.h"
#include "zmessageboxhelpers.h"
#include <QApplication>
#include <tbb/parallel_for.h>
#include <algorithm>
#include <functional>
#include <limits>

namespace nim {
namespace {

struct ZImgToQImageContext
{
  const ZImgInfo* imgInfo = nullptr;
  const std::map<size_t, std::pair<double, double>>* channels = nullptr;
  const std::map<size_t, col4>* channelColors = nullptr;
  double alpha = 1.0;
  ZImgColorizationMode colorizationMode = ZImgColorizationMode::Intensity;
};

[[nodiscard]] uint64_t splitmix64(uint64_t x)
{
  // SplitMix64 constants from Steele et al.; used for fast, deterministic hashing.
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

[[nodiscard]] inline col4 labelColorForId(uint64_t id, uint8_t alpha)
{
  // Segmentation IDs are not required to be contiguous or small. We use a deterministic hash
  // to generate a stable pseudo-random color for each ID.
  if (id == 0) {
    return col4{0, 0, 0, alpha};
  }

  const uint64_t h = splitmix64(id);

  // Avoid very dark colors by biasing RGB up.
  constexpr uint8_t kMinChannel = 64;
  constexpr uint8_t kChannelSpan = 191; // 64..255 inclusive
  const uint8_t r = static_cast<uint8_t>(kMinChannel + ((h >> 0) & 0xff) * kChannelSpan / 255);
  const uint8_t g = static_cast<uint8_t>(kMinChannel + ((h >> 8) & 0xff) * kChannelSpan / 255);
  const uint8_t b = static_cast<uint8_t>(kMinChannel + ((h >> 16) & 0xff) * kChannelSpan / 255);
  return col4{r, g, b, alpha};
}

template<typename TVoxel>
void setQImageDataBlockLabels(const ZImgToQImageContext& ctx,
                              const ZImg* img,
                              QImage* qim,
                              const tbb::blocked_range<size_t>& rowRange,
                              size_t channel)
{
  CHECK(img);
  CHECK(qim);
  CHECK(ctx.channels);
  CHECK(ctx.imgInfo);

  const uint8_t a = static_cast<uint8_t>(std::clamp(ctx.alpha, 0.0, 1.0) * 255 + 0.5);
  const TVoxel* imgData = img->rowData<TVoxel>(rowRange.begin(), 0, channel, 0);
  for (size_t i = rowRange.begin(); i != rowRange.end(); ++i) {
    QRgb* qimData = reinterpret_cast<QRgb*>(qim->scanLine(i));

    for (int j = 0; j < qim->width(); ++j) {
      const uint64_t id = static_cast<uint64_t>(*(imgData++));
      const col4 c = labelColorForId(id, a);
      qimData[j] = qRgba(c.r, c.g, c.b, c.a);
    }
  }
}

template<typename TVoxel>
[[nodiscard]] bool setQImageDataLabels(const ZImgToQImageContext& ctx, const ZImg& img, QImage& qim)
{
  if (!ctx.channels || ctx.channels->empty()) {
    return false;
  }
  if (ctx.channels->size() != 1) {
    return false;
  }

  const size_t ch = ctx.channels->begin()->first;
  if (ctx.imgInfo && ctx.imgInfo->isAlphaChannel(ch)) {
    return false;
  }

  tbb::parallel_for(tbb::blocked_range<size_t>(0, img.height()), [&](const tbb::blocked_range<size_t>& range) {
    setQImageDataBlockLabels<TVoxel>(ctx, &img, &qim, range, ch);
  });
  return true;
}

template<typename TVoxel>
void setQImageDataBlockCM([[maybe_unused]] const ZImgToQImageContext& ctx,
                          const ZImg* img,
                          QImage* qim,
                          const tbb::blocked_range<size_t>& rowRange,
                          const std::vector<size_t>* channels,
                          const std::vector<ZImgVoxelColormap<TVoxel>>* colormaps)
{
  std::vector<const TVoxel*> imgDatas(channels->size());
  for (size_t c = 0; c < channels->size(); ++c) {
    imgDatas[c] = img->rowData<TVoxel>(rowRange.begin(), 0, (*channels)[c], 0);
  }
  for (size_t i = rowRange.begin(); i != rowRange.end(); ++i) {
    QRgb* qimData = reinterpret_cast<QRgb*>(qim->scanLine(i));

    for (int j = 0; j < qim->width(); ++j) {
      col4 col = (*colormaps)[0].color(*(imgDatas[0])++);
      for (size_t c = 1; c < channels->size(); ++c) {
        col.max((*colormaps)[c].color(*(imgDatas[c])++));
      }

      qimData[j] = qRgba(col.r, col.g, col.b, col.a);
    }
  }
}

template<typename TVoxel>
void setQImageDataBlockCMMultAlpha([[maybe_unused]] const ZImgToQImageContext& ctx,
                                   const ZImg* img,
                                   QImage* qim,
                                   const tbb::blocked_range<size_t>& rowRange,
                                   const std::vector<size_t>* channels,
                                   const std::vector<ZImgVoxelColormap<TVoxel>>* colormaps)
{
  std::vector<const TVoxel*> imgDatas(channels->size());
  for (size_t c = 0; c < channels->size(); ++c) {
    imgDatas[c] = img->rowData<TVoxel>(rowRange.begin(), 0, (*channels)[c], 0);
  }
  for (size_t i = rowRange.begin(); i != rowRange.end(); ++i) {
    QRgb* qimData = reinterpret_cast<QRgb*>(qim->scanLine(i));

    for (int j = 0; j < qim->width(); ++j) {
      col4 col = (*colormaps)[0].color(*(imgDatas[0])++);
      size_t c = 1;
      for (; c < channels->size() - 1; ++c) {
        col.max((*colormaps)[c].color(*(imgDatas[c])++));
      }
      // multiply alpha channel (which contains global alpha)
      float a = (*colormaps)[c].color(*(imgDatas[c])++).a / 255.f;
      qimData[j] =
        qRgba(static_cast<int>(col.r * a + .5f), // not correct for some edge cases but faster than std::round
              static_cast<int>(col.g * a + .5f),
              static_cast<int>(col.b * a + .5f),
              static_cast<int>(col.a * a + .5f));
    }
  }
}

template<typename TVoxel>
void setQImageDataCM(const ZImgToQImageContext& ctx, const ZImg& img, QImage& qim)
{
  size_t numChannelsToShow = ctx.channels->size();
  std::vector<size_t> channels(numChannelsToShow);
  std::vector<ZImgVoxelColormap<TVoxel>> colormaps(numChannelsToShow);
  TVoxel minimum = std::numeric_limits<TVoxel>::min();
  TVoxel maximum = std::numeric_limits<TVoxel>::max();
  size_t idx = 0;
  int alphaChannelIdx = -1;
  for (const auto& chRange : *ctx.channels) {
    if (ctx.imgInfo->isAlphaChannel(chRange.first)) {
      alphaChannelIdx = static_cast<int>(idx);
      break;
    }
    idx++;
  }
  idx = 0;
  for (const auto& chRange : *ctx.channels) {
    size_t ch = chRange.first;
    double minValue = chRange.second.first;
    double maxValue = chRange.second.second;
    channels[idx] = ch;
    colormaps[idx].setRange(minimum, maximum);
    if (ctx.imgInfo->isAlphaChannel(ch)) {
      for (auto v = minimum; v <= maximum; ++v) {
        double da = (v - minValue) / (maxValue - minValue);
        da = std::clamp(da, 0.0, 1.0);
        auto value = static_cast<uint8_t>(ctx.alpha * da * 255 + 0.5);
        colormaps[idx].color(v) = col4{value, value, value, value};
        // Check if the next increment would cause an overflow
        if (v == maximum) {
          break; // Stop the loop if 'v' has reached 'maximum' to prevent incrementing beyond
        }
      }
    } else {
      col4 maxCol(ctx.channelColors->at(ch));
      if (alphaChannelIdx < 0) { // no alpha channel, encode global alpha into colormap
        maxCol.r = static_cast<uint8_t>(ctx.alpha * maxCol.r + 0.5);
        maxCol.g = static_cast<uint8_t>(ctx.alpha * maxCol.g + 0.5);
        maxCol.b = static_cast<uint8_t>(ctx.alpha * maxCol.b + 0.5);
        maxCol.a = static_cast<uint8_t>(ctx.alpha * 255 + 0.5);
        TVoxel v = minimum;
        for (; v < maximum; ++v) {
          colormaps[idx].color(v) = scaleDownColorRGB(maxCol, (v - minValue) / (maxValue - minValue));
        }
        colormaps[idx].color(v) = scaleDownColorRGB(maxCol, (v - minValue) / (maxValue - minValue));
      } else { // has alpha channel, skip global alpha as it is already encoded in alpha channel's colormap
        maxCol.a = 255;
        TVoxel v = minimum;
        for (; v < maximum; ++v) {
          colormaps[idx].color(v) = scaleDownColorRGB(maxCol, (v - minValue) / (maxValue - minValue));
        }
        colormaps[idx].color(v) = scaleDownColorRGB(maxCol, (v - minValue) / (maxValue - minValue));
      }
    }
    idx++;
  }

  if (alphaChannelIdx < 0 || ctx.channels->size() == 1) { // no alpha channel or only alpha channel
    tbb::parallel_for(tbb::blocked_range<size_t>(0, img.height()), [&](const tbb::blocked_range<size_t>& range) {
      setQImageDataBlockCM<TVoxel>(ctx, &img, &qim, range, &channels, &colormaps);
    });
  } else {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, img.height()), [&](const tbb::blocked_range<size_t>& range) {
      setQImageDataBlockCMMultAlpha<TVoxel>(ctx, &img, &qim, range, &channels, &colormaps);
    });
  }
}

template<typename TVoxel>
void setQImageDataBlock(const ZImgToQImageContext& ctx,
                        const ZImg* img,
                        QImage* qim,
                        const tbb::blocked_range<size_t>& rowRange,
                        const std::vector<size_t>* channels)
{
  std::vector<const TVoxel*> imgDatas(channels->size());
  std::vector<double> chMinValue(channels->size());
  std::vector<double> chMaxValue(channels->size());
  std::vector<col4> chCol(channels->size());
  int alphaChannelIdx = -1;
  for (size_t c = 0; c < channels->size(); ++c) {
    size_t ch = (*channels)[c];
    imgDatas[c] = img->rowData<TVoxel>(rowRange.begin(), 0, ch, 0);
    chMinValue[c] = ctx.channels->at(ch).first;
    chMaxValue[c] = ctx.channels->at(ch).second;
    chCol[c] = ctx.channelColors->at(ch);
    chCol[c].r = static_cast<uint8_t>(ctx.alpha * chCol[c].r + 0.5);
    chCol[c].g = static_cast<uint8_t>(ctx.alpha * chCol[c].g + 0.5);
    chCol[c].b = static_cast<uint8_t>(ctx.alpha * chCol[c].b + 0.5);
    chCol[c].a = static_cast<uint8_t>(ctx.alpha * 255 + 0.5);
    if (ctx.imgInfo->isAlphaChannel(ch)) {
      alphaChannelIdx = static_cast<int>(c);
    }
  }
  if (alphaChannelIdx < 0) {
    for (size_t i = rowRange.begin(); i != rowRange.end(); ++i) {
      QRgb* qimData = reinterpret_cast<QRgb*>(qim->scanLine(i));

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
        QRgb* qimData = reinterpret_cast<QRgb*>(qim->scanLine(i));

        for (int j = 0; j < qim->width(); ++j) {
          double da = (*(imgDatas[0])++ - chMinValue[0]) / (chMaxValue[0] - chMinValue[0]);
          da = da < 0.0 ? 0.0 : da > 1.0 ? 1.0 : da;
          auto v = static_cast<int>(ctx.alpha * da * 255 + 0.5);
          qimData[j] = qRgba(v, v, v, v);
        }
      }
    } else {
      for (size_t i = rowRange.begin(); i != rowRange.end(); ++i) {
        QRgb* qimData = reinterpret_cast<QRgb*>(qim->scanLine(i));

        for (int j = 0; j < qim->width(); ++j) {
          size_t c = 0;
          TVoxel v = *(imgDatas[c])++;
          col4 col = scaleDownColorRGB(chCol[c], (v - chMinValue[c]) / (chMaxValue[c] - chMinValue[c]));
          for (c = 1; c < channels->size() - 1; ++c) {
            v = *(imgDatas[c])++;
            col.max(scaleDownColorRGB(chCol[c], (v - chMinValue[c]) / (chMaxValue[c] - chMinValue[c])));
          }
          // multiply alpha channel
          double a = (*(imgDatas[c])++ - chMinValue[c]) / (chMaxValue[c] - chMinValue[c]);
          a = a < 0.0 ? 0.0 : a > 1.0 ? 1.0 : a;
          qimData[j] =
            qRgba(static_cast<int>(col.r * a + .5), // not correct for some edge cases but faster than std::round
                  static_cast<int>(col.g * a + .5),
                  static_cast<int>(col.b * a + .5),
                  static_cast<int>(col.a * a + .5));
        }
      }
    }
  }
}

template<typename TVoxel>
void setQImageData(const ZImgToQImageContext& ctx, const ZImg& img, QImage& qim)
{
  std::vector<size_t> channels(ctx.channels->size());
  size_t idx = 0;
  for (const auto& chRange : *ctx.channels) {
    channels[idx++] = chRange.first;
  }

  tbb::parallel_for(tbb::blocked_range<size_t>(0, img.height()), [&](const tbb::blocked_range<size_t>& range) {
    setQImageDataBlock<TVoxel>(ctx, &img, &qim, range, &channels);
  });
}

void fillQImageFromZImgImpl(const ZImgToQImageContext& ctx, const ZImg& img, QImage& qim)
{
  if (ctx.channels->empty()) {
    return;
  }

  if (ctx.colorizationMode == ZImgColorizationMode::SegmentationLabels && img.voxelFormat() == VoxelFormat::Unsigned) {
    const size_t bytesPerVoxel = img.bytesPerVoxel();
    bool ok = false;
    switch (bytesPerVoxel) {
      case 1:
        ok = setQImageDataLabels<uint8_t>(ctx, img, qim);
        break;
      case 2:
        ok = setQImageDataLabels<uint16_t>(ctx, img, qim);
        break;
      case 4:
        ok = setQImageDataLabels<uint32_t>(ctx, img, qim);
        break;
      case 8:
        ok = setQImageDataLabels<uint64_t>(ctx, img, qim);
        break;
      default:
        break;
    }
    if (ok) {
      return;
    }
  }

  size_t bytesPerVoxel = img.bytesPerVoxel();
  VoxelFormat vf = img.voxelFormat();
  if (vf == VoxelFormat::Float) {
    switch (bytesPerVoxel) {
      case 4:
        setQImageData<float>(ctx, img, qim);
        break;
      case 8:
        setQImageData<double>(ctx, img, qim);
        break;
      default:
        break;
    }
  } else if (vf == VoxelFormat::Signed) {
    switch (bytesPerVoxel) {
      case 1:
        setQImageDataCM<int8_t>(ctx, img, qim);
        break;
      case 2:
        setQImageDataCM<int16_t>(ctx, img, qim);
        break;
      case 4:
        setQImageData<int32_t>(ctx, img, qim);
        break;
      case 8:
        setQImageData<int64_t>(ctx, img, qim);
        break;
      default:
        break;
    }
  } else {
    switch (bytesPerVoxel) {
      case 1:
        setQImageDataCM<uint8_t>(ctx, img, qim);
        break;
      case 2:
        setQImageDataCM<uint16_t>(ctx, img, qim);
        break;
      case 4:
        setQImageData<uint32_t>(ctx, img, qim);
        break;
      case 8:
        setQImageData<uint64_t>(ctx, img, qim);
        break;
      default:
        break;
    }
  }
}

} // namespace

ZImgPackDisplay::ZImgPackDisplay(const ZImgPack& imgPack)
  : m_imgPack(imgPack)
{
  reset();
}

void ZImgPackDisplay::reset()
{
  m_z = 0;
  m_t = 0;
  m_scale = 1.0;
  m_alpha = 1.0;

  m_mip = false;
  m_mipZStart = 0;
  m_mipZEnd = m_imgPack.imgInfo().depth - 1;

  hideAllChannels();
}

void ZImgPackDisplay::showChannel(size_t ch, double minData, double maxData)
{
  CHECK(minData <= maxData);

  if (minData == maxData) {
    if (minData > m_imgPack.rangeMin()) {
      minData = m_imgPack.rangeMin();
    } else {
      maxData = minData + 1.;
    }
  }

  if (ch < m_imgPack.imgInfo().numChannels) {
    m_channels[ch] = std::make_pair(minData, maxData);
  }
}

void ZImgPackDisplay::hideChannel(size_t ch)
{
  auto it = m_channels.find(ch);
  if (it != m_channels.end()) {
    m_channels.erase(it);
  }
}

void ZImgPackDisplay::showAllChannels(double minData, double maxData)
{
  CHECK(minData <= maxData);
  m_channels.clear();

  if (minData == maxData) {
    if (minData > m_imgPack.rangeMin()) {
      minData = m_imgPack.rangeMin();
    } else {
      maxData = minData + 1.;
    }
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
    ZImgDisplay display(m_mip ? m_imgPack.maxZProjectedImg(m_mipZStart, m_mipZEnd) : m_imgPack.img());
    display.setSlice(m_z);
    display.setTime(m_t);
    display.setAlpha(m_alpha);
    for (const auto& chRange : m_channels) {
      display.showChannel(chRange.first, chRange.second.first, chRange.second.second);
      display.setChannelColor(chRange.first, m_channelColors.at(chRange.first));
    }
    return display.toQImagePack(tileWidth, tileHeight);
  }

  ZQImagePack resV;
  if (m_channels.empty()) {
    return resV;
  }

  try {
    std::vector<std::shared_ptr<ZImg>> imgs;
    std::vector<QPoint> locs;
    std::vector<double> scales;

    if (m_mip) {
      m_imgPack.retrieveCoveredMIPImgs(imgs, locs, scales, m_mipZStart, m_mipZEnd, m_t, m_viewport, m_scale);
    } else {
      m_imgPack.retrieveCoveredImgs(imgs, locs, scales, m_z, m_t, m_viewport, m_scale);
    }

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
  }
  catch (const ZException& e) {
    showCriticalWithDetails(QApplication::activeWindow(), QStringLiteral("Can not compose image pack"), e.what());
  }

  return resV;
}

void ZImgPackDisplay::fillQImage(const ZImg& img, QImage& qim) const
{
  const ZImgInfo info = m_imgPack.imgInfo();
  const ZImgToQImageContext ctx{
    .imgInfo = &info,
    .channels = &m_channels,
    .channelColors = &m_channelColors,
    .alpha = m_alpha,
    .colorizationMode = m_colorizationMode,
  };
  fillQImageFromZImgImpl(ctx, img, qim);
}

ZQImagePack qImagePackFromZImgs(const std::vector<std::shared_ptr<ZImg>>& imgs,
                                const std::vector<QPoint>& locs,
                                const std::vector<double>& scales,
                                const ZImgInfo& imgInfo,
                                const std::map<size_t, std::pair<double, double>>& channels,
                                const std::map<size_t, col4>& channelColors,
                                double alpha,
                                size_t tileWidth,
                                size_t tileHeight,
                                ZImgColorizationMode colorizationMode)
{
  CHECK(imgs.size() == locs.size());
  CHECK(imgs.size() == scales.size());

  ZQImagePack out;
  if (imgs.empty() || channels.empty()) {
    return out;
  }

  const ZImgToQImageContext ctx{
    .imgInfo = &imgInfo,
    .channels = &channels,
    .channelColors = &channelColors,
    .alpha = alpha,
    .colorizationMode = colorizationMode,
  };

  for (size_t i = 0; i < imgs.size(); ++i) {
    CHECK(imgs[i]);
    if (imgs[i]->width() <= tileWidth && imgs[i]->height() <= tileHeight) {
      QImage res(imgs[i]->width(), imgs[i]->height(), QImage::Format_ARGB32_Premultiplied);
      fillQImageFromZImgImpl(ctx, *imgs[i], res);
      out.addImage(res, locs[i], scales[i]);
      continue;
    }

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
        fillQImageFromZImgImpl(ctx, croped, res);
        out.addImage(res, locs[i] + QPoint(startX, startY), scales[i]);
      }
    }
  }

  return out;
}

} // namespace nim
