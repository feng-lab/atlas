#pragma once

#include "zimg.h"

#include <QImage>

namespace nim {

class ZImgQtUtils
{
public:
  static ZImg fromQImage(const QImage& image);
};

} // namespace nim

