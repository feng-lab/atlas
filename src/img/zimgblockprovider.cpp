#include "zimgblockprovider.h"

namespace nim {

ZImg ZImgBlockProvider::wholeImg() const
{
  ZImg res;

  res.infoRef() = imgInfo();
  res.allocate();

  for (size_t b = 0; b < numBlocks(); ++b) {
    res.pasteImg(block(b), blockCoord(b));
  }

  return res;
}

} // namespace nim
