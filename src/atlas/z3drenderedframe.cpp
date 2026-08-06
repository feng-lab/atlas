#include "z3drenderedframe.h"

#include "z3dtiledescriptor.h"

namespace nim {

Z3DRenderedFrame::Z3DRenderedFrame(glm::uvec2 fullOutputExtent, bool stereoPair)
{
  CHECK_GT(fullOutputExtent.x, 0u);
  CHECK_GT(fullOutputExtent.y, 0u);
  primaryColor = ZImg(ZImgInfo(fullOutputExtent.x, fullOutputExtent.y, 1, 4));
  primaryColor.infoRef().lastChannelIsAlphaChannel = true;
  if (stereoPair) {
    rightColor = ZImg(ZImgInfo(fullOutputExtent.x, fullOutputExtent.y, 1, 4));
    rightColor->infoRef().lastChannelIsAlphaChannel = true;
  }
}

void Z3DRenderedFrame::pasteTile(const Z3DTileDescriptor& descriptor, const Z3DRenderedTile& renderedTile)
{
  const glm::uvec2 fullOutputExtent(primaryColor.width(), primaryColor.height());
  CHECK(descriptor.fullOutputExtent() == fullOutputExtent)
    << "Every tile in a rendered frame must share the full output extent";
  CHECK_EQ(renderedTile.primaryColor.width(), descriptor.validOutputExtent().x);
  CHECK_EQ(renderedTile.primaryColor.height(), descriptor.validOutputExtent().y);
  CHECK_EQ(renderedTile.primaryColor.info().numChannels, 4u);
  CHECK_EQ(renderedTile.primaryColor.info().depth, 1u);
  CHECK_EQ(renderedTile.primaryColor.info().numTimes, 1u);
  CHECK(renderedTile.primaryColor.isType<uint8_t>());
  CHECK_EQ(renderedTile.rightColor.has_value(), rightColor.has_value())
    << "Every tile in a rendered frame must use the same eye mode";

  const glm::uvec2 assemblyOrigin = descriptor.topLeftAssemblyOrigin();
  const ZVoxelCoordinate pasteOrigin(assemblyOrigin.x, assemblyOrigin.y);
  primaryColor.pasteImg(renderedTile.primaryColor, pasteOrigin);

  if (rightColor.has_value()) {
    CHECK_EQ(renderedTile.rightColor->width(), descriptor.validOutputExtent().x);
    CHECK_EQ(renderedTile.rightColor->height(), descriptor.validOutputExtent().y);
    CHECK(renderedTile.rightColor->isSameType(renderedTile.primaryColor));
    CHECK_EQ(renderedTile.rightColor->info().numChannels, 4u);
    CHECK_EQ(renderedTile.rightColor->info().depth, 1u);
    CHECK_EQ(renderedTile.rightColor->info().numTimes, 1u);
    rightColor->pasteImg(*renderedTile.rightColor, pasteOrigin);
  }
}

} // namespace nim
