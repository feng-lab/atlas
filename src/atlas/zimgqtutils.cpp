#include "zimgqtutils.h"

namespace nim {

ZImg ZImgQtUtils::fromQImage(const QImage& image)
{
  ZImg res;
  if (image.isNull()) {
    return res;
  }

  QImage qimg = image;
  if (image.format() != QImage::Format_ARGB32 && image.format() != QImage::Format_ARGB32_Premultiplied) {
    qimg = image.convertToFormat(QImage::Format_ARGB32);
  }

  ZImgInfo info(image.width(), image.height(), 1, 4);
  info.lastChannelIsAlphaChannel = true;
  res = ZImg(info);
  for (auto h = 0; h < image.height(); ++h) {
    auto qimData = reinterpret_cast<QRgb*>(qimg.scanLine(h));
    for (auto w = 0; w < qimg.width(); ++w) {
      auto rgb = qimData[w];
      res.setValue(qRed(rgb), w, h, 0, 0);
      res.setValue(qGreen(rgb), w, h, 0, 1);
      res.setValue(qBlue(rgb), w, h, 0, 2);
      res.setValue(qAlpha(rgb), w, h, 0, 3);
    }
  }

  if (qimg.format() == QImage::Format_ARGB32_Premultiplied) {
    res.correctPreMultipliedColor();
  }

  return res;
}

} // namespace nim
