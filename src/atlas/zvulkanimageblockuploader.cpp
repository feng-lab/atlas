#include "zvulkanimageblockuploader.h"

#include "z3dimg.h"

namespace nim {

void ZVulkanImageBlockUploader::bindToImage(Z3DImg& image)
{
  image.attachVulkanImageBlockUploader(this);
  ensureImageResources(image);
}

} // namespace nim
