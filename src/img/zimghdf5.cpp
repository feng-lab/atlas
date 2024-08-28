#include "zimghdf5.h"
#include "zglobal.h"
#include "zimgblockprovider.h"
#include "zimginfoio.h"
#include "zimgsliceprovider.h"
#include "zioutils.h"
#include "zlog.h"
#include "zmemorymappedfilecache.h"
#include "zbenchtimer.h"
#include "zh5zjpegxr.h"
#include "zimgjpegxr.h"
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <folly/compression/Compression.h>
#include <folly/io/IOBuf.h>
#include <utility>
#include <boost/unordered/unordered_flat_map.hpp>

DEFINE_bool(zimg_use_mmap_file_for_hdf5, false, "Whether to create mmap file for nim format, default is false");

namespace {

size_t chunkSize()
{
  return size_t(512);
}

size_t cacheSize()
{
  return size_t(1024) * 1024 * 4000;
}

H5::FileAccPropList accPropList()
{
  H5::FileAccPropList accPropList = H5::FileAccPropList::DEFAULT;
  accPropList.setCache(1000000, cacheSize() / chunkSize() / chunkSize() * 100, cacheSize(), 0.75);
  return accPropList;
}

void readH5DataToImg(nim::ZImg& img, const H5::DataSet& data, size_t x_, size_t y_)
{
  H5::FloatType doubleType(H5::PredType::IEEE_F64LE);
  H5::FloatType floatType(H5::PredType::IEEE_F32LE);
  H5::IntType uint64Type(H5::PredType::STD_U64LE);
  H5::IntType uint32Type(H5::PredType::STD_U32LE);
  H5::IntType uint16Type(H5::PredType::STD_U16LE);
  H5::IntType uint8Type(H5::PredType::STD_U8LE);
  H5::IntType int64Type(H5::PredType::STD_I64LE);
  H5::IntType int32Type(H5::PredType::STD_I32LE);
  H5::IntType int16Type(H5::PredType::STD_I16LE);
  H5::IntType int8Type(H5::PredType::STD_I8LE);

  H5::DataSpace filespace = data.getSpace();

  if (filespace.getSimpleExtentNdims() != 2) {
    throw nim::ZException("wrong slice data dimension number");
  }

  hsize_t dims[2];
  filespace.getSimpleExtentDims(dims);
  // VLOG(1) << dims[0] << " " << dims[1] << img.info() << x_ <<" "<< y_;

  if (dims[1] < img.width() + x_ || dims[0] < img.height() + y_) {
    throw nim::ZException("wrong slice data dimension");
  }

  hsize_t offset[2] = {y_, x_};
  hsize_t count[2] = {img.height(), img.width()};
  // Define the memory space to read a chunk.
  H5::DataSpace mspace(2, count);
  filespace.selectHyperslab(H5S_SELECT_SET, count, offset);

  if (img.voxelFormat() == nim::VoxelFormat::Unsigned) {
    switch (img.bytesPerVoxel()) {
      case 1:
        data.read(img.timeData(0), uint8Type, mspace, filespace);
        break;
      case 2:
        data.read(img.timeData(0), uint16Type, mspace, filespace);
        break;
      case 4:
        data.read(img.timeData(0), uint32Type, mspace, filespace);
        break;
      case 8:
        data.read(img.timeData(0), uint64Type, mspace, filespace);
        break;
      default:
        break;
    }
  } else if (img.voxelFormat() == nim::VoxelFormat::Float) {
    switch (img.bytesPerVoxel()) {
      case 4:
        data.read(img.timeData(0), floatType, mspace, filespace);
        break;
      case 8:
        data.read(img.timeData(0), doubleType, mspace, filespace);
        break;
      default:
        break;
    }
  } else if (img.voxelFormat() == nim::VoxelFormat::Signed) {
    switch (img.bytesPerVoxel()) {
      case 1:
        data.read(img.timeData(0), int8Type, mspace, filespace);
        break;
      case 2:
        data.read(img.timeData(0), int16Type, mspace, filespace);
        break;
      case 4:
        data.read(img.timeData(0), int32Type, mspace, filespace);
        break;
      case 8:
        data.read(img.timeData(0), int64Type, mspace, filespace);
        break;
      default:
        break;
    }
  }
}

void writeFixedValueImgSliceToH5Grp(H5::Group& zGrp,
                                    const H5std_string& name,
                                    const nim::ZImg& img,
                                    const nim::ZImgWriteParameters& paras)
{
  H5::FloatType doubleType(H5::PredType::IEEE_F64LE);
  H5::FloatType floatType(H5::PredType::IEEE_F32LE);
  H5::IntType uint64Type(H5::PredType::STD_U64LE);
  H5::IntType uint32Type(H5::PredType::STD_U32LE);
  H5::IntType uint16Type(H5::PredType::STD_U16LE);
  H5::IntType uint8Type(H5::PredType::STD_U8LE);
  H5::IntType int64Type(H5::PredType::STD_I64LE);
  H5::IntType int32Type(H5::PredType::STD_I32LE);
  H5::IntType int16Type(H5::PredType::STD_I16LE);
  H5::IntType int8Type(H5::PredType::STD_I8LE);
  uint64_t v0 = 0;
  int8_t v8 = std::numeric_limits<int8_t>::min();
  int16_t v16 = std::numeric_limits<int16_t>::min();
  int32_t v32 = std::numeric_limits<int32_t>::min();
  int64_t v64 = std::numeric_limits<int64_t>::min();

  hsize_t imgDim[2] = {img.height(), img.width()};
  hsize_t chunkDim[2] = {std::min(img.height(), chunkSize()), std::min(img.width(), chunkSize())};
  H5::DataSpace imgDataspace(2, imgDim);
  H5::DSetCreatPropList pList;
  pList.setChunk(2, chunkDim);
  if (paras.compression == nim::Compression::JPEGXR) {
    if (img.voxelFormat() != nim::VoxelFormat::Unsigned || img.bytesPerVoxel() > 2) {
      throw nim::ZException("image can not be compressed with jpegxr");
    }
    size_t nelements = 4;
    unsigned int values[] = {std::bit_cast<unsigned int>(float(paras.jpegXRQuality)),
                             (unsigned int)img.bytesPerVoxel(),
                             (unsigned int)chunkDim[0],
                             (unsigned int)chunkDim[1]};
    H5Pset_filter(pList.getId(), H5Z_FILTER_JPEGXR, H5Z_FLAG_OPTIONAL, nelements, values);
  } else {
    pList.setDeflate(paras.zlibCompressionLevel);
  }

  H5::DataSet imgData;

  if (img.voxelFormat() == nim::VoxelFormat::Unsigned) {
    switch (img.bytesPerVoxel()) {
      case 1:
        pList.setFillValue(uint8Type, &v0);
        imgData = zGrp.createDataSet(name, uint8Type, imgDataspace, pList);
        break;
      case 2:
        pList.setFillValue(uint16Type, &v0);
        imgData = zGrp.createDataSet(name, uint16Type, imgDataspace, pList);
        break;
      case 4:
        pList.setFillValue(uint32Type, &v0);
        imgData = zGrp.createDataSet(name, uint32Type, imgDataspace, pList);
        break;
      case 8:
        pList.setFillValue(uint64Type, &v0);
        imgData = zGrp.createDataSet(name, uint64Type, imgDataspace, pList);
        break;
      default:
        break;
    }
  } else if (img.voxelFormat() == nim::VoxelFormat::Float) {
    switch (img.bytesPerVoxel()) {
      case 4:
        pList.setFillValue(floatType, &v0);
        imgData = zGrp.createDataSet(name, floatType, imgDataspace, pList);
        break;
      case 8:
        pList.setFillValue(doubleType, &v0);
        imgData = zGrp.createDataSet(name, doubleType, imgDataspace, pList);
        break;
      default:
        break;
    }
  } else if (img.voxelFormat() == nim::VoxelFormat::Signed) {
    switch (img.bytesPerVoxel()) {
      case 1:
        pList.setFillValue(int8Type, &v8);
        imgData = zGrp.createDataSet(name, int8Type, imgDataspace, pList);
        break;
      case 2:
        pList.setFillValue(int16Type, &v16);
        imgData = zGrp.createDataSet(name, int16Type, imgDataspace, pList);
        break;
      case 4:
        pList.setFillValue(int32Type, &v32);
        imgData = zGrp.createDataSet(name, int32Type, imgDataspace, pList);
        break;
      case 8:
        pList.setFillValue(int64Type, &v64);
        imgData = zGrp.createDataSet(name, int64Type, imgDataspace, pList);
        break;
      default:
        break;
    }
  }
}

void writeImgSliceToH5Grp(H5::Group& zGrp,
                          const H5std_string& name,
                          const nim::ZImg& img,
                          const nim::ZImgWriteParameters& paras)
{
  H5::FloatType doubleType(H5::PredType::IEEE_F64LE);
  H5::FloatType floatType(H5::PredType::IEEE_F32LE);
  H5::IntType uint64Type(H5::PredType::STD_U64LE);
  H5::IntType uint32Type(H5::PredType::STD_U32LE);
  H5::IntType uint16Type(H5::PredType::STD_U16LE);
  H5::IntType uint8Type(H5::PredType::STD_U8LE);
  H5::IntType int64Type(H5::PredType::STD_I64LE);
  H5::IntType int32Type(H5::PredType::STD_I32LE);
  H5::IntType int16Type(H5::PredType::STD_I16LE);
  H5::IntType int8Type(H5::PredType::STD_I8LE);

  hsize_t imgDim[2] = {img.height(), img.width()};
  hsize_t chunkDim[2] = {std::min(img.height(), chunkSize()), std::min(img.width(), chunkSize())};
  H5::DataSpace imgDataspace(2, imgDim);
  H5::DSetCreatPropList pList;
  pList.setChunk(2, chunkDim);
  if (paras.compression == nim::Compression::JPEGXR) {
    if (img.voxelFormat() != nim::VoxelFormat::Unsigned || img.bytesPerVoxel() > 2) {
      throw nim::ZException("image can not be compressed with jpegxr");
    }
    size_t nelements = 4;
    unsigned int values[] = {std::bit_cast<unsigned int>(float(paras.jpegXRQuality)),
                             (unsigned int)img.bytesPerVoxel(),
                             (unsigned int)chunkDim[0],
                             (unsigned int)chunkDim[1]};
    H5Pset_filter(pList.getId(), H5Z_FILTER_JPEGXR, H5Z_FLAG_OPTIONAL, nelements, values);
  } else {
    pList.setDeflate(paras.zlibCompressionLevel);
  }

  H5::DataSet imgData;

  if (img.voxelFormat() == nim::VoxelFormat::Unsigned) {
    switch (img.bytesPerVoxel()) {
      case 1:
        imgData = zGrp.createDataSet(name, uint8Type, imgDataspace, pList);
        imgData.write(img.timeData<uint8_t>(0), uint8Type);
        break;
      case 2:
        imgData = zGrp.createDataSet(name, uint16Type, imgDataspace, pList);
        imgData.write(img.timeData<uint16_t>(0), uint16Type);
        break;
      case 4:
        imgData = zGrp.createDataSet(name, uint32Type, imgDataspace, pList);
        imgData.write(img.timeData<uint32_t>(0), uint32Type);
        break;
      case 8:
        imgData = zGrp.createDataSet(name, uint64Type, imgDataspace, pList);
        imgData.write(img.timeData<uint64_t>(0), uint64Type);
        break;
      default:
        break;
    }
  } else if (img.voxelFormat() == nim::VoxelFormat::Float) {
    switch (img.bytesPerVoxel()) {
      case 4:
        imgData = zGrp.createDataSet(name, floatType, imgDataspace, pList);
        imgData.write(img.timeData<float>(0), floatType);
        break;
      case 8:
        imgData = zGrp.createDataSet(name, doubleType, imgDataspace, pList);
        imgData.write(img.timeData<double>(0), doubleType);
        break;
      default:
        break;
    }
  } else if (img.voxelFormat() == nim::VoxelFormat::Signed) {
    switch (img.bytesPerVoxel()) {
      case 1:
        imgData = zGrp.createDataSet(name, int8Type, imgDataspace, pList);
        imgData.write(img.timeData<int8_t>(0), int8Type);
        break;
      case 2:
        imgData = zGrp.createDataSet(name, int16Type, imgDataspace, pList);
        imgData.write(img.timeData<int16_t>(0), int16Type);
        break;
      case 4:
        imgData = zGrp.createDataSet(name, int32Type, imgDataspace, pList);
        imgData.write(img.timeData<int32_t>(0), int32Type);
        break;
      case 8:
        imgData = zGrp.createDataSet(name, int64Type, imgDataspace, pList);
        imgData.write(img.timeData<int64_t>(0), int64Type);
        break;
      default:
        break;
    }
  }
}

void mergeImgToH5DataSetMax(H5::DataSet& imgData,
                            const nim::ZVoxelCoordinate imgDataCoord,
                            const nim::ZImg& img,
                            const nim::ZVoxelCoordinate imgCoord)
{
  CHECK(imgDataCoord.x == 0 && imgDataCoord.y == 0 && imgDataCoord.z >= 0 && imgDataCoord.c >= 0 &&
        imgDataCoord.t >= 0);
  CHECK(imgCoord.x >= 0 && imgCoord.y >= 0 && imgCoord.z >= 0 && imgCoord.c >= 0 && imgCoord.t >= 0);
  nim::ZImgInfo info = img.info();
  info.numTimes = 1;
  info.numChannels = 1;
  info.depth = 1;
  nim::ZImg currentImg(info);
  readH5DataToImg(currentImg, imgData, imgCoord.x, imgCoord.y);
  nim::ZVoxelCoordinate off = imgCoord - imgDataCoord;
  off.x = 0;
  off.y = 0;
  currentImg.pasteImgMax(img, off);

  H5::FloatType doubleType(H5::PredType::IEEE_F64LE);
  H5::FloatType floatType(H5::PredType::IEEE_F32LE);
  H5::IntType uint64Type(H5::PredType::STD_U64LE);
  H5::IntType uint32Type(H5::PredType::STD_U32LE);
  H5::IntType uint16Type(H5::PredType::STD_U16LE);
  H5::IntType uint8Type(H5::PredType::STD_U8LE);
  H5::IntType int64Type(H5::PredType::STD_I64LE);
  H5::IntType int32Type(H5::PredType::STD_I32LE);
  H5::IntType int16Type(H5::PredType::STD_I16LE);
  H5::IntType int8Type(H5::PredType::STD_I8LE);

  H5::DataSpace filespace = imgData.getSpace();

  if (filespace.getSimpleExtentNdims() != 2) {
    throw nim::ZException("wrong slice data dimension number");
  }

  hsize_t dims[2];
  filespace.getSimpleExtentDims(dims);
  // VLOG(1) << dims[0] << " " << dims[1] << img.info() << x_ <<" "<< y_;

  if (dims[1] < img.width() + imgCoord.x || dims[0] < img.height() + imgCoord.y) {
    throw nim::ZException("wrong slice data dimension");
  }

  hsize_t offset[2] = {hsize_t(imgCoord.y), hsize_t(imgCoord.x)};
  hsize_t count[2] = {img.height(), img.width()};
  // Define the memory space to read a chunk.
  H5::DataSpace mspace(2, count);
  filespace.selectHyperslab(H5S_SELECT_SET, count, offset);

  if (currentImg.voxelFormat() == nim::VoxelFormat::Unsigned) {
    switch (currentImg.bytesPerVoxel()) {
      case 1:
        imgData.write(currentImg.timeData<uint8_t>(0), uint8Type, mspace, filespace);
        break;
      case 2:
        imgData.write(currentImg.timeData<uint16_t>(0), uint16Type, mspace, filespace);
        break;
      case 4:
        imgData.write(currentImg.timeData<uint32_t>(0), uint32Type, mspace, filespace);
        break;
      case 8:
        imgData.write(currentImg.timeData<uint64_t>(0), uint64Type, mspace, filespace);
        break;
      default:
        break;
    }
  } else if (currentImg.voxelFormat() == nim::VoxelFormat::Float) {
    switch (currentImg.bytesPerVoxel()) {
      case 4:
        imgData.write(currentImg.timeData<float>(0), floatType, mspace, filespace);
        break;
      case 8:
        imgData.write(currentImg.timeData<double>(0), doubleType, mspace, filespace);
        break;
      default:
        break;
    }
  } else if (currentImg.voxelFormat() == nim::VoxelFormat::Signed) {
    switch (currentImg.bytesPerVoxel()) {
      case 1:
        imgData.write(currentImg.timeData<int8_t>(0), int8Type, mspace, filespace);
        break;
      case 2:
        imgData.write(currentImg.timeData<int16_t>(0), int16Type, mspace, filespace);
        break;
      case 4:
        imgData.write(currentImg.timeData<int32_t>(0), int32Type, mspace, filespace);
        break;
      case 8:
        imgData.write(currentImg.timeData<int64_t>(0), int64Type, mspace, filespace);
        break;
      default:
        break;
    }
  }
}

std::set<size_t> loadRatiosFromH5Grp(const H5::Group& grp)
{
  H5::IntType uint64Type(H5::PredType::STD_U64LE);
  uint64_t numLevels;
  std::vector<uint64_t> levels;

  {
    H5::Attribute attr = grp.openAttribute("NumberOfResolutionLevels");
    attr.read(uint64Type, &numLevels);
  }

  {
    H5::Attribute attr = grp.openAttribute("DownsamplingFactors");
    if (attr.getSpace().getSimpleExtentNdims() != 1) {
      throw nim::ZException("wrong levels dimension number");
    }
    hsize_t dims[1];
    attr.getSpace().getSimpleExtentDims(dims);
    if (dims[0] > 0) {
      levels.resize(dims[0]);
      attr.read(uint64Type, levels.data());
    }
  }

  if (numLevels != levels.size() || levels.empty() || levels[0] != 1) {
    throw nim::ZException("invalid levels");
  }

  std::set<size_t> res;
  res.insert(levels.begin(), levels.end());
  return res;
}

void writeRatiosToGrp(H5::Group& grp, const std::set<size_t>& ratios)
{
  H5::IntType uint64Type(H5::PredType::STD_U64LE);
  std::vector<uint64_t> levels;
  levels.insert(levels.end(), ratios.begin(), ratios.end());
  uint64_t numLevels = levels.size();

  {
    H5::DataSpace ds(H5S_SCALAR);
    H5::Attribute attr = grp.createAttribute("NumberOfResolutionLevels", uint64Type, ds);
    attr.write(uint64Type, &numLevels);
  }

  {
    hsize_t dims[1] = {levels.size()};
    H5::DataSpace ds(1, dims);
    H5::Attribute attr = grp.createAttribute("DownsamplingFactors", uint64Type, ds);
    attr.write(uint64Type, levels.data());
  }
}

} // namespace

namespace nim {

boost::unordered_flat_map<std::tuple<size_t, size_t, size_t, size_t, size_t, size_t>, HDF5ChunkInfo>
parseHDF5Chunks(const QString& filename)
{
  // level, t, c, z, y, x
  boost::unordered_flat_map<std::tuple<size_t, size_t, size_t, size_t, size_t, size_t>, HDF5ChunkInfo> res;

  if (ZImgGlobal::instance().resourcesDIR.isEmpty()) {
    return res;
  }

#ifdef _WIN32
  QString program = ZImgGlobal::instance().resourcesDIR + QString("/h5ls.exe");
#else
  QString program = ZImgGlobal::instance().resourcesDIR + QString("/h5ls");
#endif
  if (!QFile::exists(program)) {
    LOG(ERROR) << "can not find h5ls in resources folder " << ZImgGlobal::instance().resourcesDIR;
    return res;
  }
  QStringList arguments;
  arguments << "-v"
            << "-a"
            << "-r" << filename;
  LOG(INFO) << program << " " << arguments.join(" ");
  QProcess printChunkInfos;
  printChunkInfos.start(program, arguments);
  if (!printChunkInfos.waitForStarted()) {
    LOG(ERROR) << "can not start h5ls";
    return res;
  }
  if (!printChunkInfos.waitForFinished(-1)) {
    LOG(ERROR) << "h5ls error";
    return res;
  }

  static QRegularExpression dataset(R"(^/Img/TimePoint(\d+)/Channel(\d+)/Z(\d+)/Data\s+Dataset\s+.*)");
  static QRegularExpression dsDataset(
    R"(^/Img/TimePoint(\d+)/Channel(\d+)/Z(\d+)/DownsampledBy(\d+)Data\s+Dataset\s+.*)");
  static QRegularExpression filter(R"(^\s*Filter-(\d+)\:\s+(.*))");
  static QRegularExpression chunk(R"(^\s*([xa-fA-F0-9]+)\s+(\d+)\s+(\d+)\s+\[(\d+)[,\s]+(\d+)[,\s]+(\d+)[,\s]*\])");

  auto result = printChunkInfos.readAllStandardOutput();
  QTextStream in(result, QIODevice::ReadOnly);

  QString line;
  bool dataSetStarted = false;
  std::tuple<size_t, size_t, size_t, size_t> currentDataset;
  auto currentCompression = Compression::AUTO;
  bool ok1, ok2, ok3, ok4;
  while (in.readLineInto(&line)) {
    auto match = dataset.match(line);
    if (match.hasMatch()) {
      dataSetStarted = true;
      currentDataset = std::make_tuple<size_t, size_t, size_t, size_t>(1,
                                                                       match.captured(1).toUInt(&ok1),
                                                                       match.captured(2).toUInt(&ok2),
                                                                       match.captured(3).toUInt(&ok3));
      CHECK(ok1 && ok2 && ok3) << line << ok1 << ok2 << ok3;
      continue;
    }
    match = dsDataset.match(line);
    if (match.hasMatch()) {
      dataSetStarted = true;
      currentDataset = std::make_tuple<size_t, size_t, size_t, size_t>(match.captured(4).toUInt(&ok4),
                                                                       match.captured(1).toUInt(&ok1),
                                                                       match.captured(2).toUInt(&ok2),
                                                                       match.captured(3).toUInt(&ok3));
      CHECK(ok1 && ok2 && ok3 && ok4) << line << ok1 << ok2 << ok3 << ok4;
      continue;
    }
    match = filter.match(line);
    if (match.hasMatch()) {
      if (match.captured(2).contains("jpegxr")) {
        currentCompression = Compression::JPEGXR;
      } else {
        currentCompression = Compression::AUTO;
      }
      continue;
    }
    match = chunk.match(line);
    if (match.hasMatch()) {
      if (!dataSetStarted) {
        LOG(ERROR) << "parse hdf5 chunk error: " << line;
        res.clear();
        return res;
      }
      uint32_t filterMask = match.captured(1).toUInt(&ok1, 0);
      CHECK(ok1) << match.captured(0);
      HDF5ChunkInfo info;
      info.compressed = ~filterMask & uint32_t(1); // check if the first filter, i.e. the compression filter, is skipped
      info.compression = currentCompression;
      info.length = match.captured(2).toULongLong(&ok1);
      info.offset = match.captured(3).toULongLong(&ok2);
      size_t y = match.captured(4).toULongLong(&ok3);
      size_t x = match.captured(5).toULongLong(&ok4);
      CHECK(ok1 && ok2 && ok3 && ok4) << line << ok1 << ok2 << ok3 << ok4;
      auto insertRes = res.insert({std::make_tuple(std::get<0>(currentDataset),
                                                   std::get<1>(currentDataset),
                                                   std::get<2>(currentDataset),
                                                   std::get<3>(currentDataset),
                                                   y,
                                                   x),
                                   info});
      CHECK(insertRes.second) << line;
      continue;
    }
  }
  LOG(INFO) << "done";

  return res;
}

ZImgHDF5SubBlock::ZImgHDF5SubBlock(QString fileName,
                                   std::vector<std::string> tiles,
                                   const ZImgInfo& info,
                                   size_t ratio_,
                                   size_t t_,
                                   size_t z_,
                                   size_t x_,
                                   size_t y_,
                                   size_t chunkWidth,
                                   size_t chunkHeight)
  : ZImgSubBlock(t_, x_ * ratio_, y_ * ratio_, z_, info.width * ratio_, info.height * ratio_, 1, ratio_, ratio_, 1)
  , m_filename(std::move(fileName))
  , m_tiles(std::move(tiles))
  , m_info(info)
  , m_ratio(ratio_)
  , m_x(x_)
  , m_y(y_)
{
  CHECK(m_tiles.size() == m_info.numChannels) << m_tiles.size() << " " << m_info.numChannels;
  // m_codec = folly::io::getCodec(folly::io::CodecType::ZLIB, folly::io::COMPRESSION_LEVEL_DEFAULT);
  m_chunkImgInfo = m_info;
  m_chunkImgInfo.width = chunkWidth;
  m_chunkImgInfo.height = chunkHeight;
  if (FLAGS_zimg_use_mmap_file_for_hdf5) {
    m_mmf = ZMemoryMappedFileCache::instance().getMemoryMappedFile(m_filename);
  }
}

std::shared_ptr<ZImg> ZImgHDF5SubBlock::read() const
{
  if (m_tiles.empty() || m_info.isEmpty()) {
    throw ZException("empty hdf5 sub block");
  }
  std::shared_ptr<ZImg> res;
  try {
    if (m_hdf5Tiles.size() == m_tiles.size()) {
      //      if (true) {
      //        res = std::make_shared<ZImg>(m_info);
      //        res->fill(10000);
      //        return res;
      //      }
      if (m_emptyBlock) {
        res = std::make_shared<ZImg>(m_info);
        return res;
      }

      // ZBenchTimer bt;
      auto codec = folly::io::getCodec(folly::io::CodecType::ZLIB, folly::io::COMPRESSION_LEVEL_DEFAULT);
      // STOP_AND_LOG(bt);
      res = std::make_shared<ZImg>(m_chunkImgInfo);
      if (m_mmf) {
        for (size_t c = 0; c < m_hdf5Tiles.size(); ++c) {
          auto& hdf5Tile = m_hdf5Tiles[c];
          if (hdf5Tile.offset == 0 && hdf5Tile.length == 0) {
            continue;
          }
          if (hdf5Tile.compressed) {
            m_mmf->readToBuffer(hdf5Tile.offset, hdf5Tile.length, res->channelData(c));
            // bt.pause();
            if (hdf5Tile.compression == Compression::JPEGXR) {
              auto memBuf = std::make_unique_for_overwrite<std::byte[]>(res->channelByteNumber());
              ZImgJpegXR::readMemImg(res->channelData(c), hdf5Tile.length, memBuf.get(), res->channelByteNumber());
              std::memcpy(res->channelData(c), memBuf.get(), res->channelByteNumber());
            } else {
              auto ioBuf = folly::IOBuf::wrapBuffer(res->channelData(c), hdf5Tile.length);
              // VLOG(1) << hdf5Tile.length << " " << res->channelByteNumber() << " " << ioBuf->empty();
              // VLOG(1) << codec->canUncompress(ioBuf.get(), res->channelByteNumber());
              // VLOG(1) << m_x << " " << m_y << " " << m_ratio;
              // VLOG(1) << m_info;
              // VLOG(1) << codec->getUncompressedLength(ioBuf.get(), res->channelByteNumber()).value_or(0);
              // auto decompressedBuf = codec->uncompress(ioBuf.get());
              // VLOG(1) << decompressedBuf->length();
              auto decompressedBuf = codec->uncompress(ioBuf.get(), res->channelByteNumber());
              // STOP_AND_LOG(bt)
              std::memcpy(res->channelData(c), decompressedBuf->data(), res->channelByteNumber());
            }
          } else {
            CHECK(res->channelByteNumber() == hdf5Tile.length) << res->channelByteNumber() << " " << hdf5Tile.length;
            m_mmf->readToBuffer(hdf5Tile.offset, hdf5Tile.length, res->channelData(c));
          }
        }
      } else {
        std::ifstream inputFileStream = openIFStream(m_filename, std::ios_base::in | std::ios_base::binary);
        for (size_t c = 0; c < m_hdf5Tiles.size(); ++c) {
          auto& hdf5Tile = m_hdf5Tiles[c];
          if (hdf5Tile.offset == 0 && hdf5Tile.length == 0) {
            continue;
          }
          inputFileStream.seekg(hdf5Tile.offset);
          if (hdf5Tile.compressed) {
            readStream(inputFileStream, res->channelData(c), hdf5Tile.length);
            if (hdf5Tile.compression == Compression::JPEGXR) {
              auto memBuf = std::make_unique_for_overwrite<std::byte[]>(res->channelByteNumber());
              ZImgJpegXR::readMemImg(res->channelData(c), hdf5Tile.length, memBuf.get(), res->channelByteNumber());
              std::memcpy(res->channelData(c), memBuf.get(), res->channelByteNumber());
            } else {
              auto ioBuf = folly::IOBuf::wrapBuffer(res->channelData(c), hdf5Tile.length);
              // VLOG(1) << hdf5Tile.length << " " << res->channelByteNumber() << " " << ioBuf->empty();
              // VLOG(1) << codec->canUncompress(ioBuf.get(), res->channelByteNumber());
              // VLOG(1) << m_x << " " << m_y << " " << m_ratio;
              // VLOG(1) << m_info;
              // VLOG(1) << codec->getUncompressedLength(ioBuf.get(), res->channelByteNumber()).value_or(0);
              // auto decompressedBuf = codec->uncompress(ioBuf.get());
              // VLOG(1) << decompressedBuf->length();
              auto decompressedBuf = codec->uncompress(ioBuf.get(), res->channelByteNumber());
              std::memcpy(res->channelData(c), decompressedBuf->data(), res->channelByteNumber());
            }
          } else {
            CHECK(res->channelByteNumber() == hdf5Tile.length) << res->channelByteNumber() << " " << hdf5Tile.length;
            readStream(inputFileStream, res->channelData(c), hdf5Tile.length);
          }
        }
      }
      if (m_chunkImgInfo.width != m_info.width || m_chunkImgInfo.height != m_info.height) {
        ZImg tmp(m_info);
        tmp.pasteImg(*res);
        res->swap(tmp);
      }

      return res;
    }
  }
  catch (const std::exception& e) {
    throw ZException(fmt::format("read {} folly:{}", m_filename, e.what()), ZException::Option::CheckErrno);
  }

  LOG(WARNING) << "fall back to single thread hdf5 image reading!";
  static std::mutex mutex;
  std::scoped_lock lock(mutex);
  try {
    res = std::make_shared<ZImg>(m_info);

    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(m_filename).constData(),
                    H5F_ACC_RDONLY,
                    H5::FileCreatPropList::DEFAULT,
                    accPropList());

    if (m_tiles.size() == 1) {
      // VLOG(1) << m_tiles[0] << m_info;
      H5::DataSet ds = file.openDataSet(m_tiles[0]);
      readH5DataToImg(*res, ds, m_x, m_y);
    } else {
      for (size_t i = 0; i < m_tiles.size(); ++i) {
        H5::DataSet ds = file.openDataSet(m_tiles[i]);
        ZImg img = res->createView(i, 0);
        readH5DataToImg(img, ds, m_x, m_y);
      }
    }

    return res;
  }
  catch (const H5::Exception& e) {
    throw ZException(fmt::format("read {} hdf5:{}", m_filename, e.getDetailMsg()), ZException::Option::CheckErrno);
  }
}

ZImgInfo ZImgHDF5SubBlock::readInfo() const
{
  return m_info;
}

void ZImgHDF5SubBlock::setHDF5ChunkInfos(const std::vector<HDF5ChunkInfo>& cinfos)
{
  if (!cinfos.empty()) {
    CHECK(cinfos.size() == m_info.numChannels) << cinfos.size() << " " << m_info.numChannels;
    m_hdf5Tiles = cinfos;
    m_emptyBlock = true;
    for (const auto& chunk : m_hdf5Tiles) {
      m_emptyBlock = m_emptyBlock && chunk.offset == 0 && chunk.length == 0;
    }
  }
}

void ZImgHDF5SubBlock::prefetch() const
{
  if (m_mmf) {
    for (const auto& hdf5Tile : m_hdf5Tiles) {
      if (hdf5Tile.offset == 0 && hdf5Tile.length == 0) {
        continue;
      }
      m_mmf->prefetch(hdf5Tile.offset, hdf5Tile.length);
    }
  }
}

QString ZImgHDF5::shortName() const
{
  return "HDF5 img";
}

QString ZImgHDF5::fullName() const
{
  return "HDF5 img";
}

QStringList ZImgHDF5::extensions() const
{
  QStringList res;
  res << "nim"
      << "h5";
  return res;
}

void ZImgHDF5::readInfo(const QString& filename,
                        std::vector<ZImgInfo>& infos,
                        std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  boost::unordered_flat_map<std::tuple<size_t, size_t, size_t, size_t, size_t, size_t>, HDF5ChunkInfo> hdf5Chunks;
  if (subBlocks) {
    if (FLAGS_zimg_use_mmap_file_for_hdf5) {
      ZMemoryMappedFileCache::instance().getOrCreateMemoryMappedFile(filename);
    }
    hdf5Chunks = parseHDF5Chunks(filename);
  }

  try {
    std::set<size_t> levels;
    HDF5ChunkInfo defaultChunk; // no actual chunk in hdf5, value is filled with data_type_min
    size_t chunkHeight = chunkSize();
    size_t chunkWidth = chunkSize();
    {
      static std::mutex mutex;
      std::scoped_lock lock(mutex);

      H5::Exception::dontPrint();

      H5::H5File file(QFile::encodeName(filename).constData(),
                      H5F_ACC_RDONLY,
                      H5::FileCreatPropList::DEFAULT,
                      accPropList());

      H5::Group allGrp = file.openGroup("Img");

      H5::IntType int32Type(H5::PredType::STD_I32LE);
      H5::Attribute verattr = allGrp.openAttribute("Version");
      int32_t ver;
      verattr.read(int32Type, &ver);

      infos.resize(1);
      infos[0] = ZImgInfoIO::load(allGrp);

      // createDefaultSubBlocks(filename, infos, subBlocks);

      levels = loadRatiosFromH5Grp(allGrp);

      //      if (subBlocks) {
      //        if (!infos[0].isEmpty()) {
      //          H5::DataSet ds0 = allGrp.openDataSet("TimePoint0/Channel0/Z0/Data");
      //          H5::DSetCreatPropList pList = ds0.getCreatePlist();
      //          hsize_t chunk_dims[2];
      //          auto rank_chunk = pList.getChunk(2, chunk_dims);
      //          if (rank_chunk != 2) {
      //            throw ZException(fmt::format("invalid rank of chunk dim {}", rank_chunk));
      //          }
      //          chunkHeight = chunk_dims[0];
      //          chunkWidth = chunk_dims[1];
      //        }
      //      }
    }

    if (subBlocks) {
      subBlocks->resize(infos.size());
      auto& subBlock = subBlocks->at(0);
      if (!infos[0].isEmpty()) {
        for (auto level : levels) {
          size_t width = std::ceil(infos[0].width * 1.0 / level);
          size_t height = std::ceil(infos[0].height * 1.0 / level);
          ZImgInfo inf = infos[0];
          inf.depth = 1;
          inf.numTimes = 1;
          for (size_t x = 0; x < width; x += chunkWidth) {
            inf.width = std::min<size_t>(chunkWidth, width - x);
            for (size_t y = 0; y < height; y += chunkHeight) {
              inf.height = std::min<size_t>(chunkHeight, height - y);
              for (size_t t = 0; t < infos[0].numTimes; ++t) {
                for (size_t z = 0; z < infos[0].depth; ++z) {
                  std::vector<std::string> tiles;
                  std::vector<HDF5ChunkInfo> chunkInfos;
                  for (size_t c = 0; c < inf.numChannels; ++c) {
                    if (level == 1) {
                      tiles.push_back(fmt::format("/Img/TimePoint{}/Channel{}/Z{}/Data", t, c, z));
                    } else {
                      tiles.push_back(
                        fmt::format("/Img/TimePoint{}/Channel{}/Z{}/DownsampledBy{}Data", t, c, z, level));
                    }
                    if (!hdf5Chunks.empty()) {
                      // VLOG(1) << level << " " << t << " " << c << " " << z << " " << y << " " << x;
                      auto key = std::make_tuple(level, t, c, z, y, x);
                      auto iter = hdf5Chunks.find(key);
                      if (iter != hdf5Chunks.end()) {
                        chunkInfos.push_back(iter->second);
                      } else {
                        chunkInfos.push_back(defaultChunk);
                      }
                    }
                  }
                  auto hdf5SubBlock = std::make_shared<ZImgHDF5SubBlock>(filename,
                                                                         tiles,
                                                                         inf,
                                                                         level,
                                                                         t,
                                                                         z,
                                                                         x,
                                                                         y,
                                                                         std::min(chunkWidth, width),
                                                                         std::min(chunkHeight, height));
                  hdf5SubBlock->setHDF5ChunkInfos(chunkInfos);
                  subBlock.push_back(hdf5SubBlock);
                }
              }
            }
          }
        }
      }
    }
  }
  catch (const H5::Exception& e) {
    throw ZException(fmt::format("hdf5:{}", e.getDetailMsg()), ZException::Option::CheckErrno);
  }
  catch (const std::exception& e) {
    throw ZException(fmt::format("std:{}", e.what()), ZException::Option::CheckErrno);
  }
}

void ZImgHDF5::readMetadata(const QString& /*filename*/, ZImgMetadata& /*meta*/, size_t scene)
{
  if (scene != 0) {
    throw ZException("invalid scene");
  }
}

void ZImgHDF5::readThumbnail(const QString& /*filename*/,
                             ZImgThumbernail& /*thumbnail*/,
                             const ZImgRegion& /*region*/,
                             size_t scene)
{
  if (scene != 0) {
    throw ZException("invalid scene");
  }
}

void ZImgHDF5::readImg(const QString& filename,
                       ZImg& img,
                       const ZImgRegion& region,
                       size_t scene,
                       size_t xRatio,
                       size_t yRatio,
                       size_t zRatio)
{
  if (scene != 0) {
    throw ZException("invalid scene");
  }
  static std::mutex mutex;
  std::scoped_lock lock(mutex);
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(),
                    H5F_ACC_RDONLY,
                    H5::FileCreatPropList::DEFAULT,
                    accPropList());

    H5::Group allGrp = file.openGroup("Img");

    H5::IntType int32Type(H5::PredType::STD_I32LE);
    H5::Attribute verattr = allGrp.openAttribute("Version");
    int32_t ver;
    verattr.read(int32Type, &ver);

    ZImgInfo info = ZImgInfoIO::load(allGrp);

    if (region.isEmpty() || !region.isValid(info)) {
      throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", info, region));
    }

    ZImgRegion rgn = region;
    rgn.resolveRegionEnd(info);

    std::set<size_t> pyRatios = loadRatiosFromH5Grp(allGrp);

    CHECK(xRatio >= 1 && yRatio >= 1 && zRatio >= 1);
    size_t readRatio = 0;
    for (auto r : pyRatios) {
      if (r <= xRatio && r <= yRatio) {
        readRatio = r;
      } else {
        break;
      }
    }

    double scale = 1.0 / readRatio;

    ZImgInfo resInfo = rgn.clip(info);
    if (readRatio > 1) {
      resInfo.width = std::ceil(resInfo.width * scale);
      resInfo.height = std::ceil(resInfo.height * scale);
      resInfo.voxelSizeX /= scale;
      resInfo.voxelSizeY /= scale;
    }
    img = ZImg(resInfo);

    std::string datasetName = readRatio == 1 ? std::string("Data") : fmt::format("DownsampledBy{}Data", readRatio);

    for (size_t t = 0; t < info.numTimes; ++t) {
      if (!rgn.tInRegion(t)) {
        continue;
      }
      H5::Group timeGrp = allGrp.openGroup(fmt::format("TimePoint{}", t));
      for (size_t c = 0; c < info.numChannels; ++c) {
        if (!rgn.cInRegion(c)) {
          continue;
        }
        H5::Group channelGrp = timeGrp.openGroup(fmt::format("Channel{}", c));
        for (size_t z = 0; z < info.depth; ++z) {
          if (!rgn.zInRegion(z)) {
            continue;
          }
          H5::Group zGrp = channelGrp.openGroup(fmt::format("Z{}", z));

          H5::DataSet data = zGrp.openDataSet(datasetName);
          ZImg desImg = img.createView(z - rgn.start.z, c - rgn.start.c, t - rgn.start.t);
          readH5DataToImg(desImg, data, std::round(rgn.start.x * scale), std::round(rgn.start.y * scale));
        }
      }
    }

    if (xRatio != readRatio || yRatio != readRatio || zRatio > 1) {
      img.zoom(1.0 * readRatio / xRatio, 1.0 * readRatio / yRatio, 1.0 / zRatio);
    }
  }
  catch (const H5::Exception& e) {
    throw ZException(fmt::format("hdf5:{}", e.getDetailMsg()), ZException::Option::CheckErrno);
  }
}

void ZImgHDF5::writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, img.info(), paras);
  static std::mutex mutex;
  std::scoped_lock lock(mutex);
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(),
                    H5F_ACC_TRUNC,
                    H5::FileCreatPropList::DEFAULT,
                    accPropList());

    H5::Group allGrp = file.createGroup("Img");

    H5::IntType int32Type(H5::PredType::STD_I32LE);
    H5::DataSpace attrDataSpace(H5S_SCALAR);
    H5::Attribute ver = allGrp.createAttribute("Version", int32Type, attrDataSpace);
    int32_t h5ImagVer = 100;
    ver.write(int32Type, &h5ImagVer);

    ZImgInfoIO::save(allGrp, img.info());

    // uint64_t numLevels = 1;
    std::set<size_t> levels{1};
    size_t level = 1;
    size_t width = img.width();
    size_t height = img.height();
    while (width > chunkSize() || height > chunkSize()) {
      // ++numLevels;
      level *= 2;
      levels.insert(level);
      width = std::ceil(width / 2.0);
      height = std::ceil(height / 2.0);
    }

    writeRatiosToGrp(allGrp, levels);

    for (size_t t = 0; t < img.numTimes(); ++t) {
      H5::Group timeGrp = allGrp.createGroup(fmt::format("TimePoint{}", t));
      for (size_t c = 0; c < img.numChannels(); ++c) {
        H5::Group channelGrp = timeGrp.createGroup(fmt::format("Channel{}", c));
        for (size_t z = 0; z < img.depth(); ++z) {
          H5::Group zGrp = channelGrp.createGroup(fmt::format("Z{}", z));

          ZImg tmpImg = img.createView(z, c, t);
          writeImgSliceToH5Grp(zGrp, "Data", tmpImg, paras);

          level = 1;
          while (tmpImg.width() > chunkSize() || tmpImg.height() > chunkSize()) {
            level *= 2;
            tmpImg.zoom(0.5, 0.5);
            writeImgSliceToH5Grp(zGrp, fmt::format("DownsampledBy{}Data", level), tmpImg, paras);
          }
        }
      }
    }
  }
  catch (const H5::Exception& e) {
    QFile::remove(filename);
    throw ZException(fmt::format("hdf5:{}", e.getDetailMsg()), ZException::Option::CheckErrno);
  }
}

void ZImgHDF5::writeImg(const QString& filename,
                        const ZImgSliceProvider& imgSliceProvider,
                        const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, imgSliceProvider.imgInfo(), paras);
  static std::mutex mutex;
  std::scoped_lock lock(mutex);
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(),
                    H5F_ACC_TRUNC,
                    H5::FileCreatPropList::DEFAULT,
                    accPropList());

    H5::IntType uint64Type(H5::PredType::STD_U64LE);

    H5::Group allGrp = file.createGroup("Img");

    H5::IntType int32Type(H5::PredType::STD_I32LE);
    H5::DataSpace attrDataSpace(H5S_SCALAR);
    H5::Attribute ver = allGrp.createAttribute("Version", int32Type, attrDataSpace);
    int32_t h5ImagVer = 100;
    ver.write(int32Type, &h5ImagVer);

    const ZImgInfo& info = imgSliceProvider.imgInfo();
    ZImgInfoIO::save(allGrp, info);

    std::set<size_t> levels{1};
    size_t level = 1;
    size_t width = info.width;
    size_t height = info.height;
    while (width > chunkSize() || height > chunkSize()) {
      level *= 2;
      levels.insert(level);
      width = std::ceil(width / 2.0);
      height = std::ceil(height / 2.0);
    }
    writeRatiosToGrp(allGrp, levels);

    for (size_t t = 0; t < imgSliceProvider.imgInfo().numTimes; ++t) {
      H5::Group timeGrp = allGrp.createGroup(fmt::format("TimePoint{}", t));
      std::vector<H5::Group> channelGrps;
      for (size_t c = 0; c < imgSliceProvider.imgInfo().numChannels; ++c) {
        channelGrps.push_back(timeGrp.createGroup(fmt::format("Channel{}", c)));
      }
      for (size_t z = 0; z < imgSliceProvider.imgInfo().depth; ++z) {
        ZImg img = imgSliceProvider.slice(z, t);
        for (size_t c = 0; c < imgSliceProvider.imgInfo().numChannels; ++c) {
          H5::Group zGrp = channelGrps[c].createGroup(fmt::format("Z{}", z));

          ZImg tmpImg = img.createView(c, 0);
          writeImgSliceToH5Grp(zGrp, "Data", tmpImg, paras);

          level = 1;
          while (tmpImg.width() > chunkSize() || tmpImg.height() > chunkSize()) {
            level *= 2;
            tmpImg.zoom(0.5, 0.5);
            writeImgSliceToH5Grp(zGrp, fmt::format("DownsampledBy{}Data", level), tmpImg, paras);
          }
        }
        LOG(INFO) << "Finish slice " << z << "/" << imgSliceProvider.imgInfo().depth;
      }
      LOG(INFO) << "Finish time " << t << "/" << imgSliceProvider.imgInfo().numTimes;
    }
  }
  catch (const H5::Exception& e) {
    QFile::remove(filename);
    throw ZException(fmt::format("hdf5:{}", e.getDetailMsg()), ZException::Option::CheckErrno);
  }
}

void ZImgHDF5::writeImg(const QString& filename,
                        const ZImgBlockProvider& imgBlockrovider,
                        const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, imgBlockrovider.imgInfo(), paras);
  static std::mutex mutex;
  std::scoped_lock lock(mutex);
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(),
                    H5F_ACC_TRUNC,
                    H5::FileCreatPropList::DEFAULT,
                    accPropList());

    H5::IntType uint64Type(H5::PredType::STD_U64LE);

    H5::Group allGrp = file.createGroup("Img");

    H5::IntType int32Type(H5::PredType::STD_I32LE);
    H5::DataSpace attrDataSpace(H5S_SCALAR);
    H5::Attribute ver = allGrp.createAttribute("Version", int32Type, attrDataSpace);
    int32_t h5ImagVer = 100;
    ver.write(int32Type, &h5ImagVer);

    const ZImgInfo& info = imgBlockrovider.imgInfo();
    ZImgInfoIO::save(allGrp, info);

    // uint64_t numLevels = 1;
    std::set<size_t> levels{1};
    size_t level = 1;
    size_t width = info.width;
    size_t height = info.height;
    while (width > chunkSize() || height > chunkSize()) {
      // ++numLevels;
      level *= 2;
      levels.insert(level);
      width = std::ceil(width / 2.0);
      height = std::ceil(height / 2.0);
    }
    writeRatiosToGrp(allGrp, levels);

    // write all zero image
    ZImgRegion rgn(0, -1, 0, -1, 0, 1, 0, 1, 0, 1);
    ZImgInfo sliceInfo = rgn.clip(info);
    ZImg imgSlice(sliceInfo);
    for (size_t t = 0; t < imgBlockrovider.imgInfo().numTimes; ++t) {
      H5::Group timeGrp = allGrp.createGroup(fmt::format("TimePoint{}", t));
      for (size_t c = 0; c < imgBlockrovider.imgInfo().numChannels; ++c) {
        H5::Group channelGrp = timeGrp.createGroup(fmt::format("Channel{}", c));
        for (size_t z = 0; z < imgBlockrovider.imgInfo().depth; ++z) {
          H5::Group zGrp = channelGrp.createGroup(fmt::format("Z{}", z));

          writeFixedValueImgSliceToH5Grp(zGrp, "Data", imgSlice, paras);
        }
      }
    }

    // update hdf5 file with blocks
    for (size_t i = 0; i < imgBlockrovider.numBlocks(); ++i) {
      ZVoxelCoordinate imgCoord = imgBlockrovider.blockCoord(i);
      ZImg img = imgBlockrovider.block(i);
      for (size_t t = imgCoord.t; t < imgCoord.t + img.info().numTimes; ++t) {
        for (size_t c = imgCoord.c; c < imgCoord.c + img.info().numChannels; ++c) {
          for (size_t z = imgCoord.z; z < imgCoord.z + img.info().depth; ++z) {
            auto dataLoc = fmt::format("/Img/TimePoint{}/Channel{}/Z{}/Data", t, c, z);
            H5::DataSet imgData = file.openDataSet(dataLoc);
            mergeImgToH5DataSetMax(imgData, ZVoxelCoordinate(0, 0, z, c, t), img, imgCoord);
          }
        }
      }
      LOG(INFO) << "Finish block " << i << "/" << imgBlockrovider.numBlocks();
    }

    // create pyramidal
    for (size_t t = 0; t < imgBlockrovider.imgInfo().numTimes; ++t) {
      for (size_t c = 0; c < imgBlockrovider.imgInfo().numChannels; ++c) {
        for (size_t z = 0; z < imgBlockrovider.imgInfo().depth; ++z) {
          auto grpLoc = fmt::format("/Img/TimePoint{}/Channel{}/Z{}", t, c, z);
          auto dataLoc = fmt::format("/Img/TimePoint{}/Channel{}/Z{}/Data", t, c, z);
          H5::DataSet ds = file.openDataSet(dataLoc);
          H5::Group zGrp = file.openGroup(grpLoc);
          readH5DataToImg(imgSlice, ds, 0, 0);

          ZImg tmpImg = imgSlice.createView();

          level = 1;
          while (tmpImg.width() > chunkSize() || tmpImg.height() > chunkSize()) {
            level *= 2;
            tmpImg.zoom(0.5, 0.5);
            writeImgSliceToH5Grp(zGrp, fmt::format("DownsampledBy{}Data", level), tmpImg, paras);
          }

          LOG(INFO) << "Finish building pyramidal for slice " << z << " of channel " << c;
        }
      }
    }
  }
  catch (const H5::Exception& e) {
    QFile::remove(filename);
    throw ZException(fmt::format("hdf5:{}", e.getDetailMsg()), ZException::Option::CheckErrno);
  }
}

bool ZImgHDF5::supportRead() const
{
  return true;
}

bool ZImgHDF5::supportWrite() const
{
  return true;
}

} // namespace nim
