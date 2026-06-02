#include "zimghdf5.h"
#include "zcommandlineflags.h"
#include "zstringutils.h"
#include "zimgblockprovider.h"
#include "zimginfoio.h"
#include "zimgsliceprovider.h"
#include "zioutils.h"
#include "zlog.h"
#include "zmemorymappedfilecache.h"
#include "zbenchtimer.h"
#include "zhdf5globallock.h"
#include "zh5zjpegxr.h"
#include "zh5zzstd.h"
#include "zimgjpegxr.h"
#include <QFile>
#include <folly/compression/Compression.h>
#include <zstd.h>
#include <array>
#include <bit>
#include <limits>
#include <string>
#include <utility>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/regex.hpp>
#include <set>
#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

ABSL_FLAG(bool, zimg_use_mmap_file_for_hdf5, false, "Whether to create mmap file for nim format, default is false");

namespace {

using namespace std::literals::string_view_literals;

using HDF5DatasetKey = std::tuple<size_t, size_t, size_t, size_t>;
using HDF5ChunkKey = std::tuple<size_t, size_t, size_t, size_t, size_t, size_t>;
using HDF5ChunkMap = boost::unordered_flat_map<HDF5ChunkKey, nim::HDF5ChunkInfo>;

struct HDF5ChunkDiscovery
{
  HDF5ChunkMap chunks;
  std::set<HDF5DatasetKey> datasets;
};

size_t chunkSize()
{
  return static_cast<size_t>(512);
}

size_t cacheSize()
{
  return static_cast<size_t>(1024) * 1024 * 4000;
}

H5::FileAccPropList accPropList()
{
  H5::FileAccPropList accPropList = H5::FileAccPropList::DEFAULT;
  accPropList.setCache(1000000, cacheSize() / chunkSize() / chunkSize() * 100, cacheSize(), 0.75);
  return accPropList;
}

std::string formatChunkKey(const HDF5ChunkKey& key)
{
  return fmt::format("(level={}, t={}, c={}, z={}, y={}, x={})",
                     std::get<0>(key),
                     std::get<1>(key),
                     std::get<2>(key),
                     std::get<3>(key),
                     std::get<4>(key),
                     std::get<5>(key));
}

std::string formatDatasetKey(const HDF5DatasetKey& key)
{
  return fmt::format("(level={}, t={}, c={}, z={})",
                     std::get<0>(key),
                     std::get<1>(key),
                     std::get<2>(key),
                     std::get<3>(key));
}

bool parseAtlasHDF5DatasetKey(std::string_view datasetName, HDF5DatasetKey& key)
{
  static const boost::regex dataset(R"(^Img/TimePoint(\d+)/Channel(\d+)/Z(\d+)/Data$)");
  static const boost::regex dsDataset(R"(^Img/TimePoint(\d+)/Channel(\d+)/Z(\d+)/DownsampledBy(\d+)Data$)");

  boost::match_results<std::string_view::iterator> match;
  if (boost::regex_match(datasetName.begin(), datasetName.end(), match, dataset)) {
    std::get<0>(key) = 1;
    nim::stringToValue(match[1].first, match[1].second, std::get<1>(key));
    nim::stringToValue(match[2].first, match[2].second, std::get<2>(key));
    nim::stringToValue(match[3].first, match[3].second, std::get<3>(key));
    return true;
  }
  if (boost::regex_match(datasetName.begin(), datasetName.end(), match, dsDataset)) {
    nim::stringToValue(match[4].first, match[4].second, std::get<0>(key));
    nim::stringToValue(match[1].first, match[1].second, std::get<1>(key));
    nim::stringToValue(match[2].first, match[2].second, std::get<2>(key));
    nim::stringToValue(match[3].first, match[3].second, std::get<3>(key));
    return true;
  }

  return false;
}

std::string_view normalizedObjectName(const char* name)
{
  std::string_view datasetName(name == nullptr ? "" : name);
  if (datasetName == "."sv) {
    return {};
  }
  if (datasetName.starts_with("./"sv)) {
    datasetName.remove_prefix(2);
  }
  if (datasetName.starts_with("/"sv)) {
    datasetName.remove_prefix(1);
  }
  return datasetName;
}

size_t checkedHDF5AddressToSize(haddr_t value, std::string_view fieldName)
{
  if (value == HADDR_UNDEF) {
    throw nim::ZException(fmt::format("undefined HDF5 chunk {}", fieldName));
  }
  if (value > std::numeric_limits<size_t>::max()) {
    throw nim::ZException(fmt::format("HDF5 chunk {} does not fit in size_t", fieldName));
  }
  return static_cast<size_t>(value);
}

size_t checkedHDF5SizeToSize(hsize_t value, std::string_view fieldName)
{
  if (value > std::numeric_limits<size_t>::max()) {
    throw nim::ZException(fmt::format("HDF5 chunk {} does not fit in size_t", fieldName));
  }
  return static_cast<size_t>(value);
}

struct DatasetFilterInfo
{
  bool supported = true;
  bool usesFilter = false;
  nim::Compression compression = nim::Compression::AUTO;
  std::string unsupportedReason;
};

DatasetFilterInfo datasetFilterInfo(hid_t dcplId, std::string_view datasetName)
{
  DatasetFilterInfo res;
  int filterCount = H5Pget_nfilters(dcplId);
  if (filterCount < 0) {
    throw nim::ZException(fmt::format("failed to query filters for HDF5 dataset {}", datasetName));
  }
  if (filterCount == 0) {
    return res;
  }
  if (filterCount != 1) {
    res.supported = false;
    res.unsupportedReason =
      fmt::format("dataset {} has {} filters; the direct chunk reader only supports zero filters or one compression "
                  "filter",
                  datasetName,
                  filterCount);
    return res;
  }

  unsigned flags = 0;
  std::array<unsigned int, 20> cdValues{};
  size_t cdValueCount = cdValues.size();
  std::array<char, 256> filterName{};
  H5Z_filter_t filterId =
    H5Pget_filter2(dcplId, 0, &flags, &cdValueCount, cdValues.data(), filterName.size(), filterName.data(), nullptr);
  if (filterId < 0) {
    throw nim::ZException(fmt::format("failed to query filter for HDF5 dataset {}", datasetName));
  }

  res.usesFilter = true;
  if (filterId == H5Z_FILTER_DEFLATE) {
    res.compression = nim::Compression::DEFLATE;
  } else if (filterId == H5Z_FILTER_JPEGXR) {
    res.compression = nim::Compression::JPEGXR;
  } else if (filterId == H5Z_FILTER_ZSTD) {
    res.compression = nim::Compression::ZSTD;
  } else {
    res.supported = false;
    res.unsupportedReason = fmt::format("dataset {} uses unsupported HDF5 filter id {} ({})",
                                        datasetName,
                                        static_cast<int>(filterId),
                                        std::string_view(filterName.data()));
  }

  return res;
}

struct DatasetChunkIterState
{
  HDF5ChunkMap* chunks = nullptr;
  HDF5DatasetKey datasetKey;
  bool usesFilter = false;
  nim::Compression compression = nim::Compression::AUTO;
  std::string error;
};

int collectDatasetChunk(const hsize_t* offset, unsigned filterMask, haddr_t addr, hsize_t size, void* opData)
{
  auto* state = static_cast<DatasetChunkIterState*>(opData);
  try {
    if (state == nullptr || state->chunks == nullptr || offset == nullptr) {
      throw nim::ZException("invalid HDF5 chunk iterator state");
    }
    if (size == 0 || addr == HADDR_UNDEF) {
      return H5_ITER_CONT;
    }

    nim::HDF5ChunkInfo info;
    info.offset = checkedHDF5AddressToSize(addr, "address"sv);
    info.length = checkedHDF5SizeToSize(size, "length"sv);
    info.compressed = state->usesFilter && ((filterMask & uint32_t(1)) == 0);
    info.compression = state->compression;

    const auto key = std::make_tuple(std::get<0>(state->datasetKey),
                                     std::get<1>(state->datasetKey),
                                     std::get<2>(state->datasetKey),
                                     std::get<3>(state->datasetKey),
                                     checkedHDF5SizeToSize(offset[0], "logical y offset"sv),
                                     checkedHDF5SizeToSize(offset[1], "logical x offset"sv));
    auto insertRes = state->chunks->insert({key, info});
    if (!insertRes.second) {
      throw nim::ZException(fmt::format("duplicate HDF5 chunk metadata for {}", formatChunkKey(key)));
    }
  }
  catch (const std::exception& e) {
    state->error = e.what();
    return H5_ITER_ERROR;
  }

  return H5_ITER_CONT;
}

struct HDF5ApiVisitState
{
  H5::H5File* file = nullptr;
  HDF5ChunkMap chunks;
  std::set<HDF5DatasetKey> datasets;
  size_t matchedDatasetCount = 0;
  bool unsupported = false;
  std::string unsupportedReason;
  std::string error;
};

bool collectChunksForDataset(H5::H5File& file,
                             std::string_view datasetName,
                             const HDF5DatasetKey& datasetKey,
                             HDF5ChunkMap* chunks,
                             std::string* unsupportedReason)
{
  CHECK(chunks != nullptr);
  CHECK(unsupportedReason != nullptr);

  H5::DataSet dataSet = file.openDataSet(std::string(datasetName));
  H5::DataSpace dataSpace = dataSet.getSpace();
  const int rank = dataSpace.getSimpleExtentNdims();
  if (rank != 2) {
    *unsupportedReason =
      fmt::format("dataset {} has rank {}; the direct chunk reader expects rank 2", datasetName, rank);
    return false;
  }

  hsize_t dims[2] = {0, 0};
  dataSpace.getSimpleExtentDims(dims);

  H5::DSetCreatPropList createPlist = dataSet.getCreatePlist();
  if (H5Pget_layout(createPlist.getId()) != H5D_CHUNKED) {
    *unsupportedReason = fmt::format("dataset {} is not chunked", datasetName);
    return false;
  }

  hsize_t chunkDims[2] = {0, 0};
  const int chunkRank = H5Pget_chunk(createPlist.getId(), 2, chunkDims);
  if (chunkRank != 2) {
    *unsupportedReason =
      fmt::format("dataset {} has chunk rank {}; the direct chunk reader expects rank 2", datasetName, chunkRank);
    return false;
  }
  const hsize_t expectedChunkY = std::min<hsize_t>(dims[0], chunkSize());
  const hsize_t expectedChunkX = std::min<hsize_t>(dims[1], chunkSize());
  if (chunkDims[0] != expectedChunkY || chunkDims[1] != expectedChunkX) {
    *unsupportedReason = fmt::format("dataset {} has chunk dims [{}, {}], expected [{}, {}]",
                                     datasetName,
                                     chunkDims[0],
                                     chunkDims[1],
                                     expectedChunkY,
                                     expectedChunkX);
    return false;
  }

  DatasetFilterInfo filter = datasetFilterInfo(createPlist.getId(), datasetName);
  if (!filter.supported) {
    *unsupportedReason = filter.unsupportedReason;
    return false;
  }

  DatasetChunkIterState iterState;
  iterState.chunks = chunks;
  iterState.datasetKey = datasetKey;
  iterState.usesFilter = filter.usesFilter;
  iterState.compression = filter.compression;
  if (H5Dchunk_iter(dataSet.getId(), H5P_DEFAULT, collectDatasetChunk, &iterState) < 0) {
    throw nim::ZException(iterState.error.empty()
                            ? fmt::format("failed to iterate HDF5 chunks for dataset {}", datasetName)
                            : iterState.error);
  }

  return true;
}

herr_t visitHDF5ObjectForChunks(hid_t /*obj*/, const char* name, const H5O_info2_t* info, void* opData)
{
  auto* state = static_cast<HDF5ApiVisitState*>(opData);
  try {
    if (state == nullptr || state->file == nullptr || info == nullptr) {
      throw nim::ZException("invalid HDF5 object visitor state");
    }
    if (state->unsupported) {
      return H5_ITER_STOP;
    }
    if (info->type != H5O_TYPE_DATASET) {
      return H5_ITER_CONT;
    }

    std::string_view datasetName = normalizedObjectName(name);
    if (datasetName.empty()) {
      return H5_ITER_CONT;
    }

    HDF5DatasetKey datasetKey;
    if (!parseAtlasHDF5DatasetKey(datasetName, datasetKey)) {
      return H5_ITER_CONT;
    }

    ++state->matchedDatasetCount;
    if (!collectChunksForDataset(*state->file, datasetName, datasetKey, &state->chunks, &state->unsupportedReason)) {
      state->unsupported = true;
      return H5_ITER_STOP;
    }
    const auto insertRes = state->datasets.insert(datasetKey);
    if (!insertRes.second) {
      throw nim::ZException(fmt::format("duplicate Atlas HDF5 dataset key {}", formatDatasetKey(datasetKey)));
    }
  }
  catch (const std::exception& e) {
    state->error = e.what();
    return H5_ITER_ERROR;
  }

  return H5_ITER_CONT;
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

void setHDF5CompressionFilter(H5::DSetCreatPropList& pList,
                              const nim::ZImg& img,
                              const nim::ZImgWriteParameters& paras,
                              const hsize_t* chunkDim)
{
  CHECK(chunkDim != nullptr);
  if (paras.compression == nim::Compression::JPEGXR) {
    if (img.voxelFormat() != nim::VoxelFormat::Unsigned || img.bytesPerVoxel() > 2) {
      throw nim::ZException("image can not be compressed with jpegxr");
    }
    size_t nelements = 4;
    unsigned int values[] = {std::bit_cast<unsigned int>(float(paras.jpegXRQuality)),
                             static_cast<unsigned int>(img.bytesPerVoxel()),
                             static_cast<unsigned int>(chunkDim[0]),
                             static_cast<unsigned int>(chunkDim[1])};
    H5Pset_filter(pList.getId(), H5Z_FILTER_JPEGXR, H5Z_FLAG_OPTIONAL, nelements, values);
  } else if (paras.compression == nim::Compression::AUTO || paras.compression == nim::Compression::ZSTD) {
    if (paras.zstdCompressionLevel < ZSTD_minCLevel() || paras.zstdCompressionLevel > ZSTD_maxCLevel()) {
      throw nim::ZException(fmt::format("invalid Zstd compression level: {}. Expected {}-{}",
                                        paras.zstdCompressionLevel,
                                        ZSTD_minCLevel(),
                                        ZSTD_maxCLevel()));
    }
    size_t nelements = 1;
    unsigned int values[] = {std::bit_cast<unsigned int>(paras.zstdCompressionLevel)};
    H5Pset_filter(pList.getId(), H5Z_FILTER_ZSTD, H5Z_FLAG_OPTIONAL, nelements, values);
  } else {
    pList.setDeflate(paras.zlibCompressionLevel);
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
  setHDF5CompressionFilter(pList, img, paras, chunkDim);

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
  setHDF5CompressionFilter(pList, img, paras, chunkDim);

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

  hsize_t offset[2] = {static_cast<hsize_t>(imgCoord.y), static_cast<hsize_t>(imgCoord.x)};
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

HDF5ChunkDiscovery parseHDF5Chunks(const QString& filename)
{
  HDF5ChunkDiscovery res;

  try {
    std::scoped_lock lock(hdf5GlobalMutex());
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(),
                    H5F_ACC_RDONLY,
                    H5::FileCreatPropList::DEFAULT,
                    accPropList());

    HDF5ApiVisitState visitState;
    visitState.file = &file;

    const herr_t visitResult =
      H5Ovisit3(file.getId(), H5_INDEX_NAME, H5_ITER_NATIVE, visitHDF5ObjectForChunks, &visitState, H5O_INFO_BASIC);
    if (!visitState.error.empty()) {
      throw ZException(visitState.error);
    }
    if (visitResult < 0) {
      throw ZException("failed to visit HDF5 objects for chunk metadata");
    }
    if (visitState.unsupported) {
      LOG(WARNING) << "HDF5 API chunk discovery cannot use the direct chunk reader for " << filename << ": "
                   << visitState.unsupportedReason << ". Falling back to serialized HDF5 reads.";
      return res;
    }

    res.chunks = std::move(visitState.chunks);
    res.datasets = std::move(visitState.datasets);
    LOG(INFO) << "HDF5 API chunk discovery found " << res.chunks.size() << " allocated chunks across "
              << visitState.matchedDatasetCount << " Atlas datasets in " << filename;
  }
  catch (const H5::Exception& e) {
    LOG(ERROR) << "HDF5 API chunk discovery failed for " << filename << ": " << e.getDetailMsg()
               << ". Falling back to serialized HDF5 reads.";
    res = HDF5ChunkDiscovery();
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "HDF5 API chunk discovery failed for " << filename << ": " << e.what()
               << ". Falling back to serialized HDF5 reads.";
    res = HDF5ChunkDiscovery();
  }

  return res;
}

namespace {

struct HDF5ImageReadPlan
{
  ZImgInfo fileInfo;
  ZImgRegion region;
  ZImgInfo readInfo;
  size_t readRatio = 1;
  size_t levelWidth = 0;
  size_t levelHeight = 0;
  size_t readX = 0;
  size_t readY = 0;
  std::string datasetName;
};

struct HDF5RawChunkReadJob
{
  HDF5ChunkInfo chunkInfo;
  size_t copyX = 0;
  size_t copyY = 0;
  size_t copyWidth = 0;
  size_t copyHeight = 0;
  size_t destX = 0;
  size_t destY = 0;
  size_t destZ = 0;
  size_t destC = 0;
  size_t destT = 0;
};

struct HDF5RawChunkReadScratch
{
  std::unique_ptr<std::ifstream> inputStream;
  ZImg chunkImg;
  std::vector<uint8_t> compressedBuffer;
  std::unique_ptr<folly::compression::StreamCodec> zlibStreamCodec;
};

size_t ceilDiv(size_t value, size_t divisor)
{
  CHECK(divisor > 0);
  return (value + divisor - 1) / divisor;
}

size_t selectHDF5ReadRatio(const std::set<size_t>& ratios, size_t xRatio, size_t yRatio)
{
  CHECK(xRatio >= 1 && yRatio >= 1);

  size_t readRatio = 0;
  for (auto r : ratios) {
    if (r <= xRatio && r <= yRatio) {
      readRatio = r;
    } else {
      break;
    }
  }
  CHECK(readRatio >= 1);
  return readRatio;
}

HDF5ImageReadPlan makeHDF5ImageReadPlan(const QString& filename, const ZImgRegion& region, size_t xRatio, size_t yRatio)
{
  std::scoped_lock lock(hdf5GlobalMutex());
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

  HDF5ImageReadPlan plan;
  plan.fileInfo = ZImgInfoIO::load(allGrp);
  if (region.isEmpty() || !region.isValid(plan.fileInfo)) {
    throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", plan.fileInfo, region));
  }

  plan.region = region;
  plan.region.resolveRegionEnd(plan.fileInfo);

  std::set<size_t> levels = loadRatiosFromH5Grp(allGrp);
  plan.readRatio = selectHDF5ReadRatio(levels, xRatio, yRatio);
  plan.levelWidth = ceilDiv(plan.fileInfo.width, plan.readRatio);
  plan.levelHeight = ceilDiv(plan.fileInfo.height, plan.readRatio);
  plan.datasetName = plan.readRatio == 1 ? std::string("Data") : fmt::format("DownsampledBy{}Data", plan.readRatio);

  const double scale = 1.0 / plan.readRatio;
  plan.readX = static_cast<size_t>(std::round(plan.region.start.x * scale));
  plan.readY = static_cast<size_t>(std::round(plan.region.start.y * scale));

  plan.readInfo = plan.region.clip(plan.fileInfo);
  if (plan.readRatio > 1) {
    plan.readInfo.width = std::ceil(plan.readInfo.width * scale);
    plan.readInfo.height = std::ceil(plan.readInfo.height * scale);
    plan.readInfo.voxelSizeX /= scale;
    plan.readInfo.voxelSizeY /= scale;
  }

  return plan;
}

void checkRequestedHDF5DatasetsDiscovered(const HDF5ImageReadPlan& plan, const std::set<HDF5DatasetKey>& datasets)
{
  for (auto t = plan.region.start.t; t < plan.region.end.t; ++t) {
    for (auto z = plan.region.start.z; z < plan.region.end.z; ++z) {
      for (auto c = plan.region.start.c; c < plan.region.end.c; ++c) {
        const HDF5DatasetKey datasetKey =
          std::make_tuple(plan.readRatio, static_cast<size_t>(t), static_cast<size_t>(c), static_cast<size_t>(z));
        if (!datasets.contains(datasetKey)) {
          throw ZException(fmt::format("HDF5 API chunk discovery did not find requested Atlas dataset {}",
                                       formatDatasetKey(datasetKey)));
        }
      }
    }
  }
}

void checkHDF5DatasetDiscovered(const std::set<HDF5DatasetKey>& datasets, size_t level, size_t t, size_t c, size_t z)
{
  const HDF5DatasetKey datasetKey = std::make_tuple(level, t, c, z);
  CHECK(datasets.contains(datasetKey)) << "HDF5 API chunk discovery did not find requested Atlas dataset "
                                       << formatDatasetKey(datasetKey);
}

void readRawHDF5Bytes(const QString& filename,
                      const ZMemoryMappedFile* mmf,
                      std::istream* inputFileStream,
                      size_t offset,
                      size_t length,
                      void* buffer)
{
  if (mmf != nullptr) {
    mmf->readToBuffer(offset, length, buffer);
    return;
  }

  std::ifstream localInputFileStream;
  if (inputFileStream == nullptr) {
    localInputFileStream = openIFStream(filename, std::ios_base::in | std::ios_base::binary);
    inputFileStream = &localInputFileStream;
  }
  inputFileStream->seekg(offset);
  readStream(*inputFileStream, static_cast<uint8_t*>(buffer), length);
}

void readRawHDF5ChunkToImg(const QString& filename,
                           const ZMemoryMappedFile* mmf,
                           std::istream* inputFileStream,
                           const HDF5ChunkInfo& hdf5Tile,
                           ZImg& img,
                           HDF5RawChunkReadScratch& scratch)
{
  if (hdf5Tile.offset == 0 && hdf5Tile.length == 0) {
    return;
  }

  if (!hdf5Tile.compressed) {
    CHECK(img.timeByteNumber() == hdf5Tile.length) << img.timeByteNumber() << " " << hdf5Tile.length;
    readRawHDF5Bytes(filename, mmf, inputFileStream, hdf5Tile.offset, hdf5Tile.length, img.timeData(0));
    return;
  }

  scratch.compressedBuffer.resize(hdf5Tile.length);
  readRawHDF5Bytes(filename, mmf, inputFileStream, hdf5Tile.offset, hdf5Tile.length, scratch.compressedBuffer.data());

  if (hdf5Tile.compression == Compression::JPEGXR) {
    ZImgJpegXR::readMemImg(scratch.compressedBuffer.data(), hdf5Tile.length, img.timeData(0), img.timeByteNumber());
    return;
  }
  if (hdf5Tile.compression == Compression::ZSTD) {
    const size_t decompressedSize =
      ZSTD_decompress(img.timeData(0), img.timeByteNumber(), scratch.compressedBuffer.data(), hdf5Tile.length);
    if (ZSTD_isError(decompressedSize) != 0) {
      throw ZException(fmt::format("zstd decompress failed: {}", ZSTD_getErrorName(decompressedSize)));
    }
    CHECK(decompressedSize == img.timeByteNumber()) << decompressedSize << " " << img.timeByteNumber();
    return;
  }

  if (!scratch.zlibStreamCodec) {
    scratch.zlibStreamCodec = folly::compression::getStreamCodec(folly::compression::CodecType::ZLIB,
                                                                 folly::compression::COMPRESSION_LEVEL_DEFAULT);
  }
  scratch.zlibStreamCodec->resetStream(static_cast<uint64_t>(img.timeByteNumber()));
  folly::ByteRange input(scratch.compressedBuffer.data(), hdf5Tile.length);
  folly::MutableByteRange output(img.timeData(0), img.timeByteNumber());
  const bool finished =
    scratch.zlibStreamCodec->uncompressStream(input, output, folly::compression::StreamCodec::FlushOp::END);
  CHECK(finished);
  CHECK(input.empty()) << input.size();
  CHECK(output.empty()) << output.size();
}

void copyRawHDF5ChunkOverlapToImage(const ZImg& chunkImg, const HDF5RawChunkReadJob& job, ZImg& img)
{
  const size_t rowByteNumber = job.copyWidth * img.bytesPerVoxel();
  for (size_t y = 0; y < job.copyHeight; ++y) {
    std::copy_n(chunkImg.data(job.copyX, job.copyY + y, 0, 0, 0),
                rowByteNumber,
                img.data(job.destX, job.destY + y, job.destZ, job.destC, job.destT));
  }
}

std::vector<HDF5RawChunkReadJob> makeRawChunkReadJobs(const HDF5ImageReadPlan& plan, const HDF5ChunkMap& hdf5Chunks)
{
  const size_t rawChunkWidth = std::min(chunkSize(), plan.levelWidth);
  const size_t rawChunkHeight = std::min(chunkSize(), plan.levelHeight);
  CHECK(rawChunkWidth > 0 && rawChunkHeight > 0);

  const size_t readXEnd = plan.readX + plan.readInfo.width;
  const size_t readYEnd = plan.readY + plan.readInfo.height;
  const size_t firstChunkX = (plan.readX / chunkSize()) * chunkSize();
  const size_t firstChunkY = (plan.readY / chunkSize()) * chunkSize();

  std::vector<HDF5RawChunkReadJob> jobs;
  for (auto t = plan.region.start.t; t < plan.region.end.t; ++t) {
    for (auto z = plan.region.start.z; z < plan.region.end.z; ++z) {
      for (auto c = plan.region.start.c; c < plan.region.end.c; ++c) {
        for (size_t chunkY = firstChunkY; chunkY < readYEnd; chunkY += chunkSize()) {
          const size_t copyYBegin = std::max(plan.readY, chunkY);
          const size_t copyYEnd = std::min(readYEnd, std::min(plan.levelHeight, chunkY + rawChunkHeight));
          if (copyYBegin >= copyYEnd) {
            continue;
          }
          for (size_t chunkX = firstChunkX; chunkX < readXEnd; chunkX += chunkSize()) {
            const size_t copyXBegin = std::max(plan.readX, chunkX);
            const size_t copyXEnd = std::min(readXEnd, std::min(plan.levelWidth, chunkX + rawChunkWidth));
            if (copyXBegin >= copyXEnd) {
              continue;
            }

            const auto key = std::make_tuple(plan.readRatio,
                                             static_cast<size_t>(t),
                                             static_cast<size_t>(c),
                                             static_cast<size_t>(z),
                                             chunkY,
                                             chunkX);
            auto chunkIt = hdf5Chunks.find(key);
            if (chunkIt == hdf5Chunks.end()) {
              continue;
            }
            if (chunkIt->second.offset == 0 && chunkIt->second.length == 0) {
              continue;
            }

            HDF5RawChunkReadJob job;
            job.chunkInfo = chunkIt->second;
            job.copyX = copyXBegin - chunkX;
            job.copyY = copyYBegin - chunkY;
            job.copyWidth = copyXEnd - copyXBegin;
            job.copyHeight = copyYEnd - copyYBegin;
            job.destX = copyXBegin - plan.readX;
            job.destY = copyYBegin - plan.readY;
            job.destZ = static_cast<size_t>(z - plan.region.start.z);
            job.destC = static_cast<size_t>(c - plan.region.start.c);
            job.destT = static_cast<size_t>(t - plan.region.start.t);
            jobs.push_back(job);
          }
        }
      }
    }
  }

  return jobs;
}

bool tryReadHDF5ImgWithRawChunks(const QString& filename,
                                 const HDF5ImageReadPlan& plan,
                                 size_t xRatio,
                                 size_t yRatio,
                                 size_t zRatio,
                                 ZImg& img)
{
  try {
    if (absl::GetFlag(FLAGS_zimg_use_mmap_file_for_hdf5)) {
      ZMemoryMappedFileCache::instance().getOrCreateMemoryMappedFile(filename);
    }
    const ZMemoryMappedFile* mmf = absl::GetFlag(FLAGS_zimg_use_mmap_file_for_hdf5)
                                     ? ZMemoryMappedFileCache::instance().getMemoryMappedFile(filename)
                                     : nullptr;

    const HDF5ChunkDiscovery chunkDiscovery = parseHDF5Chunks(filename);
    if (chunkDiscovery.datasets.empty()) {
      return false;
    }

    checkRequestedHDF5DatasetsDiscovered(plan, chunkDiscovery.datasets);
    std::vector<HDF5RawChunkReadJob> jobs = makeRawChunkReadJobs(plan, chunkDiscovery.chunks);
    ZImg res(plan.readInfo);
    if (!jobs.empty()) {
      ZImgInfo chunkInfo = plan.readInfo;
      chunkInfo.width = std::min(chunkSize(), plan.levelWidth);
      chunkInfo.height = std::min(chunkSize(), plan.levelHeight);
      chunkInfo.depth = 1;
      chunkInfo.numChannels = 1;
      chunkInfo.numTimes = 1;
      chunkInfo.createDefaultDescriptions();

      tbb::enumerable_thread_specific<HDF5RawChunkReadScratch> scratchStates([&]() {
        HDF5RawChunkReadScratch scratch;
        if (mmf != nullptr) {
          scratch.chunkImg = ZImg(chunkInfo);
          return scratch;
        }
        scratch.inputStream =
          std::make_unique<std::ifstream>(openIFStream(filename, std::ios_base::in | std::ios_base::binary));
        scratch.chunkImg = ZImg(chunkInfo);
        return scratch;
      });

      tbb::parallel_for(tbb::blocked_range<size_t>(0, jobs.size()), [&](const tbb::blocked_range<size_t>& range) {
        HDF5RawChunkReadScratch& scratch = scratchStates.local();
        std::istream* inputFileStream = scratch.inputStream.get();
        if (mmf == nullptr) {
          CHECK(inputFileStream != nullptr);
        }
        for (size_t jobIndex = range.begin(); jobIndex < range.end(); ++jobIndex) {
          const HDF5RawChunkReadJob& job = jobs[jobIndex];
          readRawHDF5ChunkToImg(filename, mmf, inputFileStream, job.chunkInfo, scratch.chunkImg, scratch);
          copyRawHDF5ChunkOverlapToImage(scratch.chunkImg, job, res);
        }
      });
    }

    if (xRatio != plan.readRatio || yRatio != plan.readRatio || zRatio > 1) {
      res.zoom(1.0 * plan.readRatio / xRatio, 1.0 * plan.readRatio / yRatio, 1.0 / zRatio);
    }
    img = std::move(res);
    return true;
  }
  catch (const std::bad_alloc&) {
    throw;
  }
  catch (const std::exception& e) {
    LOG(WARNING) << "HDF5 raw chunk image read failed for " << filename << ": " << e.what()
                 << ". Falling back to serialized HDF5 reads.";
    return false;
  }
}

void readHDF5ImgWithApi(const QString& filename,
                        const HDF5ImageReadPlan& plan,
                        size_t xRatio,
                        size_t yRatio,
                        size_t zRatio,
                        ZImg& img)
{
  std::scoped_lock lock(hdf5GlobalMutex());
  H5::Exception::dontPrint();

  H5::H5File file(QFile::encodeName(filename).constData(),
                  H5F_ACC_RDONLY,
                  H5::FileCreatPropList::DEFAULT,
                  accPropList());
  H5::Group allGrp = file.openGroup("Img");

  img = ZImg(plan.readInfo);
  for (size_t t = 0; t < plan.fileInfo.numTimes; ++t) {
    if (!plan.region.tInRegion(t)) {
      continue;
    }
    H5::Group timeGrp = allGrp.openGroup(fmt::format("TimePoint{}", t));
    for (size_t c = 0; c < plan.fileInfo.numChannels; ++c) {
      if (!plan.region.cInRegion(c)) {
        continue;
      }
      H5::Group channelGrp = timeGrp.openGroup(fmt::format("Channel{}", c));
      for (size_t z = 0; z < plan.fileInfo.depth; ++z) {
        if (!plan.region.zInRegion(z)) {
          continue;
        }
        H5::Group zGrp = channelGrp.openGroup(fmt::format("Z{}", z));
        H5::DataSet data = zGrp.openDataSet(plan.datasetName);
        ZImg desImg = img.createView(z - plan.region.start.z, c - plan.region.start.c, t - plan.region.start.t);
        readH5DataToImg(desImg, data, plan.readX, plan.readY);
      }
    }
  }

  if (xRatio != plan.readRatio || yRatio != plan.readRatio || zRatio > 1) {
    img.zoom(1.0 * plan.readRatio / xRatio, 1.0 * plan.readRatio / yRatio, 1.0 / zRatio);
  }
}

} // namespace

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
  if (absl::GetFlag(FLAGS_zimg_use_mmap_file_for_hdf5)) {
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

      res = std::make_shared<ZImg>(m_chunkImgInfo);

      HDF5RawChunkReadScratch scratch;
      if (m_mmf == nullptr) {
        scratch.inputStream =
          std::make_unique<std::ifstream>(openIFStream(m_filename, std::ios_base::in | std::ios_base::binary));
      }
      std::istream* inputFileStream = scratch.inputStream.get();
      if (m_mmf == nullptr) {
        CHECK(inputFileStream != nullptr);
      }

      for (size_t c = 0; c < m_hdf5Tiles.size(); ++c) {
        const auto& hdf5Tile = m_hdf5Tiles[c];
        if (hdf5Tile.offset == 0 && hdf5Tile.length == 0) {
          continue;
        }
        ZImg channelImg = res->createView(static_cast<index_t>(c), 0);
        readRawHDF5ChunkToImg(m_filename, m_mmf, inputFileStream, hdf5Tile, channelImg, scratch);
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
  std::scoped_lock lock(hdf5GlobalMutex());
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
  HDF5ChunkDiscovery hdf5Discovery;
  if (subBlocks) {
    if (absl::GetFlag(FLAGS_zimg_use_mmap_file_for_hdf5)) {
      ZMemoryMappedFileCache::instance().getOrCreateMemoryMappedFile(filename);
    }
    hdf5Discovery = parseHDF5Chunks(filename);
  }

  try {
    std::set<size_t> levels;
    HDF5ChunkInfo defaultChunk; // no actual chunk in hdf5, value is filled with data_type_min
    size_t chunkHeight = chunkSize();
    size_t chunkWidth = chunkSize();
    {
      std::scoped_lock lock(hdf5GlobalMutex());

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
      const bool useDirectHDF5Chunks = !hdf5Discovery.datasets.empty();
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
                    if (useDirectHDF5Chunks) {
                      checkHDF5DatasetDiscovered(hdf5Discovery.datasets, level, t, c, z);
                      // VLOG(1) << level << " " << t << " " << c << " " << z << " " << y << " " << x;
                      auto key = std::make_tuple(level, t, c, z, y, x);
                      auto iter = hdf5Discovery.chunks.find(key);
                      if (iter != hdf5Discovery.chunks.end()) {
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
                       size_t zRatio,
                       const ZImgReadOptions& /*readOptions*/)
{
  if (scene != 0) {
    throw ZException("invalid scene");
  }
  CHECK(xRatio >= 1 && yRatio >= 1 && zRatio >= 1);
  try {
    HDF5ImageReadPlan plan = makeHDF5ImageReadPlan(filename, region, xRatio, yRatio);
    if (tryReadHDF5ImgWithRawChunks(filename, plan, xRatio, yRatio, zRatio, img)) {
      return;
    }
    readHDF5ImgWithApi(filename, plan, xRatio, yRatio, zRatio, img);
  }
  catch (const H5::Exception& e) {
    throw ZException(fmt::format("hdf5:{}", e.getDetailMsg()), ZException::Option::CheckErrno);
  }
}

void ZImgHDF5::writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, img.info(), paras);
  std::scoped_lock lock(hdf5GlobalMutex());
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
  std::scoped_lock lock(hdf5GlobalMutex());
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
  std::scoped_lock lock(hdf5GlobalMutex());
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
