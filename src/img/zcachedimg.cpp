#include "zcachedimg.h"

#include "zimgio.h"
#include "zimagecompositetransform.h"
#include "zcpuinfo.h"

namespace {

template<typename ImagePixelType>
nim::ZImg transformSlice(const std::unique_ptr<nim::ZImageCompositeTransform>& tfm, const nim::ZImg& srcImg)
{
  nim::ZImg outImg = srcImg;
  for (size_t c = 0; c < srcImg.numChannels(); ++c) {
    tfm->transformImage(srcImg.planeData<ImagePixelType>(0, c),
                        srcImg.width(),
                        srcImg.height(),
                        outImg.planeData<ImagePixelType>(0, c));
  }
  return outImg;
}

} // namespace

namespace nim {

ZCachedImg::ZCachedImg(ZImgSource imgSource)
  : m_imgSource(std::move(imgSource))
{
  CHECK(m_imgSource.region.isDefault()) << "region is not supported yet";
  ZImgIO::instance().readInfo(m_imgSource, m_imgInfo);
}

ZImg ZCachedImg::slice(size_t z, size_t c, size_t t) const
{
  ZImg res;
  ZImgSource imgSource = m_imgSource;
  imgSource.region = ZImgRegion(0, -1, 0, -1, z, z + 1, c, c + 1, t, t + 1);
  ZImgIO::instance().readImg(imgSource, res);
  if (m_sliceTransform) {
    return imgTypeDispatcher(res.info(), [&, this]<typename TVoxel>() {
      return transformSlice<TVoxel>(m_sliceTransform->at(z), res);
    });
  } else {
    return res;
  }
}

ZImg ZCachedImg::slice(size_t z, size_t t) const
{
  ZImg res;
  ZImgSource imgSource = m_imgSource;
  imgSource.region = ZImgRegion(0, -1, 0, -1, z, z + 1, 0, -1, t, t + 1);
  ZImgIO::instance().readImg(imgSource, res);
  if (m_sliceTransform) {
    return imgTypeDispatcher(res.info(), [&, this]<typename TVoxel>() {
      return transformSlice<TVoxel>(m_sliceTransform->at(z), res);
    });
  } else {
    return res;
  }
}

void ZCachedImg::save(const QString& filename, FileFormat format, const ZImgWriteParameters& paras) const
{
  if (imgInfo().byteNumber() * 3 > ZCpuInfo::instance().nPhysicalRAM) {
    ZImgIO::instance().writeImg(filename, *this, format, paras);
  } else {
    wholeImg().save(filename, format, paras);
  }
}

} // namespace nim
