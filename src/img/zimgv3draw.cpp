#include "zimgv3draw.h"
#include <fstream>
#include <iostream>
#include "zioutils.h"
#include "zimgsliceprovider.h"
#include "zlog.h"

namespace nim {

ZImgV3DRaw::ZImgV3DRaw()
  : ZImgFormat()
{
}

ZImgV3DRaw::~ZImgV3DRaw()
{
}

QString ZImgV3DRaw::shortName() const
{
  return "Vaa3d raw";
}

QString ZImgV3DRaw::fullName() const
{
  return "Vaa3d raw";
}

QStringList ZImgV3DRaw::extensions() const
{
  QStringList res;
  res << "v3draw" << "raw";
  return res;
}

void ZImgV3DRaw::readInfo(const QString &filename, std::vector<ZImgInfo> &infos, std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> *subBlocks,
                          std::vector<std::set<size_t>> *pyramidalRatios)
{
  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

  char formatkey[] = "raw_image_stack_by_hpeng";
  readStream(inputFileStream, formatkey, std::strlen(formatkey));

  if (std::strcmp(formatkey, "raw_image_stack_by_hpeng") != 0) {
    throw ZIOException("File is not Vaa3D raw format.");
  }

  char endian;
  readStream(inputFileStream, &endian, 1);

  uint16_t dataType;
  readStream(inputFileStream, &dataType, 2);

  uint16_t sz_buffer[8];
  uint32_t sz[4];
  readStream(inputFileStream, sz_buffer, 8);

  for (int i = 0; i < 4; ++i) {
    sz[i] = sz_buffer[i];
  }

  if ((sz[0] == 0) || (sz[1] == 0) || (sz[2] == 0) || (sz[3] == 0)) {
    readStream(inputFileStream, sz_buffer + 4, 8);
    memcpy(sz, sz_buffer, 16);
  }

  infos.resize(1);
  infos[0].width = sz[0];
  infos[0].height = sz[1];
  infos[0].depth = sz[2];
  infos[0].numChannels = sz[3];
  infos[0].numTimes = 1;
  infos[0].bytesPerVoxel = dataType;
  if (dataType == 4 || dataType == 8)
    infos[0].voxelFormat = VoxelFormat::Float;
  else
    infos[0].voxelFormat = VoxelFormat::Unsigned;
  infos[0].createDefaultDescriptions();

  createDefaultSubBlocks(filename, infos, subBlocks, pyramidalRatios);
}

void ZImgV3DRaw::readMetadata(const QString &filename, ZImgMetadata &, size_t scene)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

  char formatkey[] = "raw_image_stack_by_hpeng";
  readStream(inputFileStream, formatkey, std::strlen(formatkey));

  if (std::strcmp(formatkey, "raw_image_stack_by_hpeng") != 0) {
    throw ZIOException("File is not Vaa3D raw format.");
  }
}

void ZImgV3DRaw::readThumbnail(const QString &filename, ZImgThumbernail &, const ZImgRegion &, size_t scene)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

  char formatkey[] = "raw_image_stack_by_hpeng";
  readStream(inputFileStream, formatkey, std::strlen(formatkey));

  if (std::strcmp(formatkey, "raw_image_stack_by_hpeng") != 0) {
    throw ZIOException("File is not Vaa3D raw format.");
  }
}

void ZImgV3DRaw::readImg(const QString &filename, ZImg &img, const ZImgRegion &region, size_t scene, size_t ratio)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

  char formatkey[] = "raw_image_stack_by_hpeng";
  readStream(inputFileStream, formatkey, std::strlen(formatkey));

  if (std::strcmp(formatkey, "raw_image_stack_by_hpeng") != 0) {
    throw ZIOException("File is not Vaa3D raw format.");
  }

  char endian;
  readStream(inputFileStream, &endian, 1);

  uint16_t dataType;
  readStream(inputFileStream, &dataType, 2);

  uint16_t sz_buffer[8];
  uint32_t sz[4];
  readStream(inputFileStream, sz_buffer, 8);

  for (int i = 0; i < 4; ++i) {
    sz[i] = sz_buffer[i];
  }
  size_t dataOffset = 35;

  if ((sz[0] == 0) || (sz[1] == 0) || (sz[2] == 0) || (sz[3] == 0)) {
    readStream(inputFileStream, sz_buffer + 4, 8);
    memcpy(sz, sz_buffer, 16);
    dataOffset += 8;
  }

  ZImgInfo imgInfo;
  imgInfo.width = sz[0];
  imgInfo.height = sz[1];
  imgInfo.depth = sz[2];
  imgInfo.numChannels = sz[3];
  imgInfo.numTimes = 1;
  imgInfo.bytesPerVoxel = dataType;
  if (dataType == 4 || dataType == 8)
    imgInfo.voxelFormat = VoxelFormat::Float;
  else
    imgInfo.voxelFormat = VoxelFormat::Unsigned;
  imgInfo.createDefaultDescriptions();

  img = readRawImg(filename, imgInfo, "ZCTL", dataOffset, region);

  if (ratio > 1) {
    img.zoom(1.0 / ratio, 1.0 / ratio);
  }
}

void ZImgV3DRaw::writeImg(const QString &filename, const ZImg &img, Compression comp)
{
  if (comp != Compression::AUTO && comp != Compression::NONE) {
    LOG(WARNING) << "compression is not supported for V3DRaw";
  }
  if (img.numTimes() != 1) {
    throw ZIOException("time sequence is not supported");
  }
  std::ofstream outputFileStream;
  openFileStream(outputFileStream, filename, std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);

  const char formatkey[] = "raw_image_stack_by_hpeng";
  writeStream(outputFileStream, formatkey, std::strlen(formatkey));

  char endian = 'L';
  writeStream(outputFileStream, &endian, 1);

  uint16_t dataType = img.voxelByteNumber();
  writeStream(outputFileStream, &dataType, 2);

  uint32_t sz[4];
  sz[0] = img.width();
  sz[1] = img.height();
  sz[2] = img.depth();
  sz[3] = img.numChannels();
  writeStream(outputFileStream, sz, 16);

  writeStream(outputFileStream, img.timeData<char>(0), img.timeByteNumber());
}

void ZImgV3DRaw::writeImg(const QString &filename, const ZImgSliceProvider &imgSliceProvider, Compression comp)
{
  if (comp != Compression::AUTO && comp != Compression::NONE) {
    LOG(WARNING) << "compression is not supported for V3DRaw";
  }
  if (imgSliceProvider.imgInfo().numTimes != 1) {
    throw ZIOException("time sequence is not supported");
  }
  if (imgSliceProvider.imgInfo().numChannels > 1 && imgSliceProvider.imgInfo().depth > 1) {
    writeImg(filename, imgSliceProvider.allSlices(0), comp);
  } else {
    std::ofstream outputFileStream;
    openFileStream(outputFileStream, filename, std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);

    const char formatkey[] = "raw_image_stack_by_hpeng";
    writeStream(outputFileStream, formatkey, std::strlen(formatkey));

    char endian = 'L';
    writeStream(outputFileStream, &endian, 1);

    uint16_t dataType = imgSliceProvider.imgInfo().voxelByteNumber();
    writeStream(outputFileStream, &dataType, 2);

    uint32_t sz[4];
    sz[0] = imgSliceProvider.imgInfo().width;
    sz[1] = imgSliceProvider.imgInfo().height;
    sz[2] = imgSliceProvider.imgInfo().depth;
    sz[3] = imgSliceProvider.imgInfo().numChannels;
    writeStream(outputFileStream, sz, 16);

    for (size_t z=0; z<imgSliceProvider.imgInfo().depth; ++z) {
      ZImg img = imgSliceProvider.slice(z,0);
      writeStream(outputFileStream, img.timeData<char>(0), img.timeByteNumber());
    }
  }
}

bool ZImgV3DRaw::supportRead() const
{
  return true;
}

bool ZImgV3DRaw::supportWrite() const
{
  return true;
}

} // namespace
