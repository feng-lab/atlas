#include "zimg.h"
#include "zh5zzstd.h"
#include "ztest.h"

#include <H5Cpp.h>
#include <QTemporaryDir>
#include <cstddef>
#include <vector>

namespace {

template<typename TVoxel>
nim::ZImg createCompressibleImage(size_t width, size_t height)
{
  nim::ZImg img(nim::ZImgInfo(width, height, 1, 1, 1, sizeof(TVoxel), nim::VoxelFormat::Unsigned));
  for (size_t y = 0; y < height; ++y) {
    for (size_t x = 0; x < width; ++x) {
      *img.data<TVoxel>(x, y) = static_cast<TVoxel>((x / 4 + y / 3) % 257);
    }
  }
  return img;
}

void expectSameInfo(const nim::ZImgInfo& lhs, const nim::ZImgInfo& rhs)
{
  EXPECT_TRUE(lhs.isSameSize(rhs));
  EXPECT_TRUE(lhs.isSameType(rhs));
  EXPECT_EQ(lhs.bytesPerVoxel, rhs.bytesPerVoxel);
  EXPECT_EQ(lhs.voxelFormat, rhs.voxelFormat);
}

H5Z_filter_t mainDataFilter(const QString& filename)
{
  H5::H5File file(filename.toStdString(), H5F_ACC_RDONLY);
  H5::DataSet dataset = file.openDataSet("Img/TimePoint0/Channel0/Z0/Data");
  H5::DSetCreatPropList properties = dataset.getCreatePlist();
  if (properties.getNfilters() != 1) {
    throw nim::ZException(fmt::format("expected one HDF5 filter, got {}", properties.getNfilters()));
  }

  unsigned int flags = 0;
  size_t cdValueCount = 8;
  unsigned int cdValues[8] = {};
  char filterName[128] = {};
  unsigned int filterConfig = 0;
  return properties.getFilter(0, flags, cdValueCount, cdValues, sizeof(filterName), filterName, filterConfig);
}

} // namespace

TEST(ImgHDF5Zstd, RoundTripsNimFile)
{
  using namespace nim;

  ASSERT_GE(zstd_register_h5filter(), 0);

  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = tmp.filePath("lossless_zstd.nim");

  ZImg source = createCompressibleImage<uint16_t>(96, 72);
  ZImgWriteParameters paras;
  paras.compression = Compression::ZSTD;
  paras.zstdCompressionLevel = 3;
  source.save(filename, FileFormat::HDF5Img, paras);

  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  const std::vector<ZImgInfo> infos = ZImg::readImgInfos(filename, &subBlocks, FileFormat::HDF5Img);
  ASSERT_EQ(infos.size(), size_t{1});
  expectSameInfo(source.info(), infos[0]);
  ASSERT_EQ(subBlocks.size(), size_t{1});
  ASSERT_EQ(subBlocks[0].size(), size_t{1});
  std::shared_ptr<ZImg> block = subBlocks[0][0]->read();
  ASSERT_TRUE(block);
  EXPECT_EQ(source, *block);

  ZImg decoded(filename, ZImgRegion(), 0, 1, 1, 1, FileFormat::HDF5Img);
  EXPECT_EQ(source, decoded);
}

TEST(ImgHDF5Zstd, AutoCompressionUsesZstd)
{
  using namespace nim;

  ASSERT_GE(zstd_register_h5filter(), 0);

  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = tmp.filePath("auto_zstd.nim");

  ZImg source = createCompressibleImage<uint16_t>(96, 72);
  ZImgWriteParameters paras;
  source.save(filename, FileFormat::HDF5Img, paras);

  EXPECT_EQ(mainDataFilter(filename), static_cast<H5Z_filter_t>(H5Z_FILTER_ZSTD));

  ZImg decoded(filename, ZImgRegion(), 0, 1, 1, 1, FileFormat::HDF5Img);
  EXPECT_EQ(source, decoded);
}
