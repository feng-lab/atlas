#include "zimghdf5.h"

#include "zioutils.h"
#include "zimgsliceprovider.h"
#include "zimgblockprovider.h"
#include "zlog.h"
#include "zimginfoio.h"
#include <QFile>
#include <QMutexLocker>
#include <utility>

#ifdef HACK_HDF5
#include "H5Dmodule.h" /* This source code file is part of the H5D module */

/***********/
/* Headers */
/***********/
#include "H5private.h" /* Generic Functions            */
#ifdef H5_HAVE_PARALLEL
#include "H5ACprivate.h" /* Metadata cache            */
#endif                   /* H5_HAVE_PARALLEL */
#include "H5CXprivate.h" /* API Contexts                         */
#include "H5Dpkg.h"      /* Dataset functions            */
#include "H5Eprivate.h"  /* Error handling              */
#include "H5Fprivate.h"  /* File functions            */
#include "H5FLprivate.h" /* Free Lists                           */
#include "H5Iprivate.h"  /* IDs                      */
#include "H5MMprivate.h" /* Memory management            */
#include "H5MFprivate.h" /* File memory management               */
#include "H5VMprivate.h" /* Vector and array functions        */
#endif

namespace {

inline size_t chunkSize()
{
  return size_t(512);
}

inline size_t cacheSize()
{
  return size_t(1024) * 1024 * 4000;
}

inline H5::FileAccPropList accPropList()
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

  if (filespace.getSimpleExtentNdims() != 2)
    throw nim::ZIOException("wrong slice data dimension number");

  hsize_t dims[2];
  filespace.getSimpleExtentDims(dims);
  //LOG(INFO) << dims[0] << " " << dims[1] << img.info().toQString().toStdString() << x_ <<" "<< y_;

  if (dims[1] < img.width() + x_ || dims[0] < img.height() + y_)
    throw nim::ZIOException("wrong slice data dimension");

  hsize_t offset[2] = {y_, x_};
  hsize_t count[2] = {img.height(), img.width()};
  //Define the memory space to read a chunk.
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

void writeFixedValueImgSliceToH5Grp(H5::Group& zGrp, const H5std_string& name, const nim::ZImg& img,
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
  hsize_t chunkDim[2] = {std::min(img.height(), chunkSize()),
                         std::min(img.width(), chunkSize())};
  H5::DataSpace imgDataspace(2, imgDim);
  H5::DSetCreatPropList pList;
  pList.setDeflate(paras.zlibCompressionLevel);
  pList.setChunk(2, chunkDim);

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

void writeImgSliceToH5Grp(H5::Group& zGrp, const H5std_string& name, const nim::ZImg& img,
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
  hsize_t chunkDim[2] = {std::min(img.height(), chunkSize()),
                         std::min(img.width(), chunkSize())};
  H5::DataSpace imgDataspace(2, imgDim);
  H5::DSetCreatPropList pList;
  pList.setDeflate(paras.zlibCompressionLevel);
  pList.setChunk(2, chunkDim);

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

void mergeImgToH5DataSetMax(H5::DataSet& imgData, const nim::ZVoxelCoordinate imgDataCoord,
                            const nim::ZImg& img, const nim::ZVoxelCoordinate imgCoord)
{
  CHECK(
    imgDataCoord.x == 0 && imgDataCoord.y == 0 && imgDataCoord.z >= 0 && imgDataCoord.c >= 0 && imgDataCoord.t >= 0);
  CHECK(
    imgCoord.x >= 0 && imgCoord.y >= 0 && imgCoord.z >= 0 && imgCoord.c >= 0 && imgCoord.t >= 0);
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

  if (filespace.getSimpleExtentNdims() != 2)
    throw nim::ZIOException("wrong slice data dimension number");

  hsize_t dims[2];
  filespace.getSimpleExtentDims(dims);
  //LOG(INFO) << dims[0] << " " << dims[1] << img.info().toQString().toStdString() << x_ <<" "<< y_;

  if (dims[1] < img.width() + imgCoord.x || dims[0] < img.height() + imgCoord.y)
    throw nim::ZIOException("wrong slice data dimension");

  hsize_t offset[2] = {hsize_t(imgCoord.y), hsize_t(imgCoord.x)};
  hsize_t count[2] = {img.height(), img.width()};
  //Define the memory space to read a chunk.
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
      throw nim::ZIOException("wrong levels dimension number");
    }
    hsize_t dims[1];
    attr.getSpace().getSimpleExtentDims(dims);
    if (dims[0] > 0) {
      levels.resize(dims[0]);
      attr.read(uint64Type, levels.data());
    }
  }

  if (numLevels != levels.size() || levels.empty() || levels[0] != 1) {
    throw nim::ZIOException("invalid levels");
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

//int print_hdf5_chunk_info(const H5D_chunk_rec_t* chunk_rec, void*)
//{
//  LOG(INFO) << chunk_rec->filter_mask << " " << chunk_rec->chunk_addr << " " << chunk_rec->nbytes << " "
//            << chunk_rec->scaled[0] << " " << chunk_rec->scaled[1];
//  return 0;
//}

}

namespace nim {

ZImgHDF5SubBlock::ZImgHDF5SubBlock(QString fileName, std::vector<std::string> tiles, const ZImgInfo& info,
                                   size_t ratio_, size_t t_, size_t z_, size_t x_, size_t y_)
  : ZImgSubBlock(t_, x_ * ratio_, y_ * ratio_, z_, info.width * ratio_, info.height * ratio_, 1, ratio_, ratio_, 1)
  , m_filename(std::move(fileName))
  , m_tiles(std::move(tiles))
  , m_info(info)
  , m_ratio(ratio_)
  , m_x(x_)
  , m_y(y_)
{
}

std::shared_ptr<ZImg> ZImgHDF5SubBlock::read() const
{
#ifndef HACK_HDF5
#else
  // todo: fix hdf5 multithread reading
  static QMutex mutex;
  QMutexLocker lock(&mutex);
  try {
    if (m_tiles.empty()) {
      throw ZIOException("empty hdf5 sub block");
    }
    auto res = std::make_shared<ZImg>(m_info);

    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(m_filename).constData(), H5F_ACC_RDONLY,
                    H5::FileCreatPropList::DEFAULT, accPropList());

    if (m_tiles.size() == 1) {
      //LOG(INFO) << m_tiles[0] << m_info.toQString();
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
  catch (H5::Exception const& e) {
    throw ZIOException(QString("read %1 hdf5:%2").arg(m_filename).arg(e.getDetailMsg().c_str()));
  }
#endif
}

ZImgInfo ZImgHDF5SubBlock::readInfo() const
{
  return m_info;
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
  res << "nim" << "h5";
  return res;
}

void ZImgHDF5::readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                        std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_RDONLY,
                    H5::FileCreatPropList::DEFAULT, accPropList());

    H5::Group allGrp = file.openGroup("Img");

    infos.resize(1);
    infos[0] = ZImgInfoIO::load(allGrp);

    //createDefaultSubBlocks(filename, infos, subBlocks);

    std::set<size_t> levels = loadRatiosFromH5Grp(allGrp);

    if (subBlocks) {
      subBlocks->resize(infos.size());
      auto& subBlock = subBlocks->at(0);
      if (!infos[0].isEmpty()) {
        H5::DataSet ds0 = allGrp.openDataSet("TimePoint0/Channel0/Z0/Data");
        H5::DSetCreatPropList pList = ds0.getCreatePlist();
        hsize_t chunk_dims[2];
        int rank_chunk = pList.getChunk(2, chunk_dims);
        if (rank_chunk != 2) {
          throw ZIOException(QString("invalid rank of chunk dim %1").arg(rank_chunk));
        }

        for (auto level : levels) {
          size_t width = std::ceil(infos[0].width * 1.0 / level);
          size_t height = std::ceil(infos[0].height * 1.0 / level);
          ZImgInfo inf = infos[0];
          inf.depth = 1;
          inf.numTimes = 1;
          for (size_t x = 0; x < width; x += chunk_dims[0]) {
            inf.width = std::min<size_t>(chunk_dims[0], width - x);
            for (size_t y = 0; y < height; y += chunk_dims[1]) {
              inf.height = std::min<size_t>(chunk_dims[1], height - y);
              for (size_t t = 0; t < infos[0].numTimes; ++t) {
                for (size_t z = 0; z < infos[0].depth; ++z) {
                  std::vector<std::string> tiles;
                  for (size_t c = 0; c < inf.numChannels; ++c) {
                    std::string tile;
                    if (level == 1) {
                      tile = QString("/Img/TimePoint%1/Channel%2/Z%3/Data").arg(t).arg(c).arg(z).toStdString();
                    } else {
                      tile = QString("/Img/TimePoint%1/Channel%2/Z%3/DownsampledBy%4Data").arg(t).arg(c).arg(z).arg(
                        level).toStdString();
                    }
                    tiles.push_back(tile);
                  }
                  auto hdf5SubBlock = std::make_shared<ZImgHDF5SubBlock>(filename, tiles, inf, level, t, z, x, y);
                  subBlock.push_back(hdf5SubBlock);

#ifdef HACK_HDF5
                  hsize_t offset[2];
                  std::vector<HDF5ChunkInfo> cinfos;
                  for (const auto& tile : tiles) {
                    H5::DataSet ds = file.openDataSet(tile);
                    //LOG(INFO) << ds.getId() << " " << ds.getObjName();
                    //LOG(INFO) << H5Pget_layout(H5Dget_create_plist(ds.getId()));
                    //H5Ddebug(ds.getId());
                    auto* dset = (H5D_t*)H5VLobject(ds.getId());
                    if (!dset) {
                      throw ZIOException("can not get dataset from hdf5, maybe wrong code");
                    }
                    // H5D__chunk_dump_index(dset, stdout);
                    CHECK(H5D_CHUNKED == dset->shared->layout.type) << dset->shared->layout.type;
                    // LOG(INFO) << dset;
                    const H5O_layout_t* layout = &(dset->shared->layout); // Dataset layout
                    hsize_t scaled[3]; // Scaled coordinates for this chunk
                    offset[0] = x;
                    offset[1] = y;
                    //LOG(INFO) << dset->shared;
                    //LOG(INFO) << dset->shared->ndims;
                    CHECK(dset->shared->ndims == 2) << dset->shared->ndims;
                    for (size_t d = 0; d < dset->shared->ndims; ++d) {
                      scaled[d] = offset[d] / layout->u.chunk.dim[d];
                    }
                    scaled[dset->shared->ndims] = 0;
                    //LOG(INFO) << scaled[0] << " " << scaled[1];

                    H5O_storage_chunk_t* sc = &(dset->shared->layout.storage.u.chunk);
                    //LOG(INFO) << sc;

                    H5D_chunk_ud_t udata;  // User data for querying chunk info
                    /* Initialize the query information about the chunk we are looking for */
                    udata.common.layout = &(dset->shared->layout.u.chunk);
                    udata.common.storage = sc;
                    udata.common.scaled = scaled;

                    /* Reset information about the chunk we are looking for */
                    udata.chunk_block.offset = HADDR_UNDEF;
                    udata.chunk_block.length = 0;
                    udata.filter_mask = 0;
                    udata.new_unfilt_chunk = FALSE;
                    udata.idx_hint = UINT_MAX;
                    //LOG(INFO) << "udata";

                    H5D_chk_idx_info_t idx_info;        /* Chunked index info */
                    /* Compose chunked index info struct */
                    idx_info.f = dset->oloc.file;
                    idx_info.pline = &dset->shared->dcpl_cache.pline;
                    idx_info.layout = &dset->shared->layout.u.chunk;
                    idx_info.storage = sc;
                    LOG(INFO) << idx_info.f;
                    LOG(INFO) << dset->shared->layout.u.chunk.ndims;

                    if ((sc->ops->dump)(sc, stdout) < 0) {
                      throw ZIOException("unable to dump chunk index info");
                    }

                    if ((sc->ops->get_addr)(&idx_info, &udata) < 0) {
                      throw ZIOException("can't query chunk address");
                    }
                    if (!H5F_addr_defined(udata.chunk_block.offset) && udata.chunk_block.length == 0) {
                      throw ZIOException("empty chunk");
                    } else if (!H5F_addr_defined(udata.chunk_block.offset)) {
                      throw ZIOException("invalid chunk");
                    }
                    HDF5ChunkInfo chunkInfo;
                    chunkInfo.offset = udata.chunk_block.offset;
                    chunkInfo.length = udata.chunk_block.length;
                    chunkInfo.compressed = ~udata.filter_mask & H5Z_FILTER_DEFLATE;
                    LOG(INFO) << chunkInfo.offset << " " << chunkInfo.length << " " << chunkInfo.compressed;
                    cinfos.push_back(chunkInfo);
                  }
                  hdf5SubBlock->setHDF5ChunkInfos(cinfos);
#endif
                }
              }
            }
          }
        }
      }
    }
  }
  catch (H5::Exception const& e) {
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
  }
}

void ZImgHDF5::readMetadata(const QString& /*filename*/, ZImgMetadata& /*meta*/, size_t scene)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
}

void ZImgHDF5::readThumbnail(const QString& /*filename*/, ZImgThumbernail& /*thumbnail*/,
                             const ZImgRegion& /*region*/, size_t scene)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
}

void ZImgHDF5::readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene,
                       size_t xRatio, size_t yRatio, size_t zRatio)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_RDONLY,
                    H5::FileCreatPropList::DEFAULT, accPropList());

    H5::Group allGrp = file.openGroup("Img");

    ZImgInfo info = ZImgInfoIO::load(allGrp);

    if (region.isEmpty() || !region.isValid(info)) {
      throw ZIOException(
        QString("Invalid image region. Image info: '%1', region: '%2'").arg(info.toQString()).arg(
          region.toQString()));
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

    std::string datasetName =
      readRatio == 1 ? std::string("Data") : QString("DownsampledBy%1Data").arg(readRatio).toStdString();

    for (size_t t = 0; t < info.numTimes; ++t) {
      if (!rgn.tInRegion(t)) {
        continue;
      }
      H5::Group timeGrp = allGrp.openGroup(qUtf8Printable(QString("TimePoint%1").arg(t)));
      for (size_t c = 0; c < info.numChannels; ++c) {
        if (!rgn.cInRegion(c)) {
          continue;
        }
        H5::Group channelGrp = timeGrp.openGroup(qUtf8Printable(QString("Channel%1").arg(c)));
        for (size_t z = 0; z < info.depth; ++z) {
          if (!rgn.zInRegion(z)) {
            continue;
          }
          H5::Group zGrp = channelGrp.openGroup(qUtf8Printable(QString("Z%1").arg(z)));

          H5::DataSet data = zGrp.openDataSet(datasetName);
          ZImg desImg = img.createView(z - rgn.start.z, c - rgn.start.c, t - rgn.start.t);
          readH5DataToImg(desImg,
                          data,
                          std::round(rgn.start.x * scale),
                          std::round(rgn.start.y * scale));
        }
      }
    }

    if (xRatio != readRatio || yRatio != readRatio || zRatio > 1) {
      img.zoom(1.0 * readRatio / xRatio, 1.0 * readRatio / yRatio, 1.0 / zRatio);
    }
  }
  catch (H5::Exception const& e) {
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
  }
}

void ZImgHDF5::writeImg(const QString& filename, const ZImg& img,
                        const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, img.info(), paras);
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_TRUNC,
                    H5::FileCreatPropList::DEFAULT, accPropList());

    H5::Group allGrp = file.createGroup("Img");

    ZImgInfoIO::save(allGrp, img.info());

    uint64_t numLevels = 1;
    std::set<size_t> levels{1};
    size_t level = 1;
    size_t width = img.width();
    size_t height = img.height();
    while (width > chunkSize() || height > chunkSize()) {
      ++numLevels;
      level *= 2;
      levels.insert(level);
      width = std::ceil(width / 2.0);
      height = std::ceil(height / 2.0);
    }

    writeRatiosToGrp(allGrp, levels);

    for (size_t t = 0; t < img.numTimes(); ++t) {
      H5::Group timeGrp = allGrp.createGroup(qUtf8Printable(QString("TimePoint%1").arg(t)));
      for (size_t c = 0; c < img.numChannels(); ++c) {
        H5::Group channelGrp = timeGrp.createGroup(qUtf8Printable(QString("Channel%1").arg(c)));
        for (size_t z = 0; z < img.depth(); ++z) {
          H5::Group zGrp = channelGrp.createGroup(qUtf8Printable(QString("Z%1").arg(z)));

          ZImg tmpImg = img.createView(z, c, t);
          writeImgSliceToH5Grp(zGrp, "Data", tmpImg, paras);

          level = 1;
          while (tmpImg.width() > chunkSize() || tmpImg.height() > chunkSize()) {
            level *= 2;
            tmpImg.zoom(0.5, 0.5);
            writeImgSliceToH5Grp(zGrp, QString("DownsampledBy%1Data").arg(level).toStdString(), tmpImg, paras);
          }
        }
      }
    }
  }
  catch (H5::Exception const& e) {
    QFile::remove(filename);
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
  }
}

void ZImgHDF5::writeImg(const QString& filename, const ZImgSliceProvider& imgSliceProvider,
                        const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, imgSliceProvider.imgInfo(), paras);
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_TRUNC,
                    H5::FileCreatPropList::DEFAULT, accPropList());

    H5::IntType uint64Type(H5::PredType::STD_U64LE);

    H5::Group allGrp = file.createGroup("Img");

    const ZImgInfo& info = imgSliceProvider.imgInfo();
    ZImgInfoIO::save(allGrp, info);

    uint64_t numLevels = 1;
    std::set<size_t> levels{1};
    size_t level = 1;
    size_t width = info.width;
    size_t height = info.height;
    while (width > chunkSize() || height > chunkSize()) {
      ++numLevels;
      level *= 2;
      levels.insert(level);
      width = std::ceil(width / 2.0);
      height = std::ceil(height / 2.0);
    }
    writeRatiosToGrp(allGrp, levels);

    for (size_t t = 0; t < imgSliceProvider.imgInfo().numTimes; ++t) {
      H5::Group timeGrp = allGrp.createGroup(qUtf8Printable(QString("TimePoint%1").arg(t)));
      std::vector<H5::Group> channelGrps;
      for (size_t c = 0; c < imgSliceProvider.imgInfo().numChannels; ++c) {
        channelGrps.push_back(timeGrp.createGroup(qUtf8Printable(QString("Channel%1").arg(c))));
      }
      for (size_t z = 0; z < imgSliceProvider.imgInfo().depth; ++z) {
        ZImg img = imgSliceProvider.slice(z, t);
        for (size_t c = 0; c < imgSliceProvider.imgInfo().numChannels; ++c) {
          H5::Group zGrp = channelGrps[c].createGroup(qUtf8Printable(QString("Z%1").arg(z)));

          ZImg tmpImg = img.createView(c, 0);
          writeImgSliceToH5Grp(zGrp, "Data", tmpImg, paras);

          level = 1;
          while (tmpImg.width() > chunkSize() || tmpImg.height() > chunkSize()) {
            level *= 2;
            tmpImg.zoom(0.5, 0.5);
            writeImgSliceToH5Grp(zGrp, QString("DownsampledBy%1Data").arg(level).toStdString(), tmpImg, paras);
          }
        }
      }
    }
  }
  catch (H5::Exception const& e) {
    QFile::remove(filename);
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
  }
}

void ZImgHDF5::writeImg(const QString& filename, const ZImgBlockProvider& imgBlockrovider,
                        const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, imgBlockrovider.imgInfo(), paras);
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_TRUNC,
                    H5::FileCreatPropList::DEFAULT, accPropList());

    H5::IntType uint64Type(H5::PredType::STD_U64LE);

    H5::Group allGrp = file.createGroup("Img");

    const ZImgInfo& info = imgBlockrovider.imgInfo();
    ZImgInfoIO::save(allGrp, info);

    uint64_t numLevels = 1;
    std::set<size_t> levels{1};
    size_t level = 1;
    size_t width = info.width;
    size_t height = info.height;
    while (width > chunkSize() || height > chunkSize()) {
      ++numLevels;
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
      H5::Group timeGrp = allGrp.createGroup(qUtf8Printable(QString("TimePoint%1").arg(t)));
      for (size_t c = 0; c < imgBlockrovider.imgInfo().numChannels; ++c) {
        H5::Group channelGrp = timeGrp.createGroup(qUtf8Printable(QString("Channel%1").arg(c)));
        for (size_t z = 0; z < imgBlockrovider.imgInfo().depth; ++z) {
          H5::Group zGrp = channelGrp.createGroup(qUtf8Printable(QString("Z%1").arg(z)));

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
            auto dataLoc = QString("/Img/TimePoint%1/Channel%2/Z%3/Data").arg(t).arg(c).arg(z).toStdString();
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
          auto grpLoc = QString("/Img/TimePoint%1/Channel%2/Z%3").arg(t).arg(c).arg(z).toStdString();
          auto dataLoc = QString("/Img/TimePoint%1/Channel%2/Z%3/Data").arg(t).arg(c).arg(z).toStdString();
          H5::DataSet ds = file.openDataSet(dataLoc);
          H5::Group zGrp = file.openGroup(grpLoc);
          readH5DataToImg(imgSlice, ds, 0, 0);

          ZImg tmpImg = imgSlice.createView();

          level = 1;
          while (tmpImg.width() > chunkSize() || tmpImg.height() > chunkSize()) {
            level *= 2;
            tmpImg.zoom(0.5, 0.5);
            writeImgSliceToH5Grp(zGrp, QString("DownsampledBy%1Data").arg(level).toStdString(), tmpImg, paras);
          }

          LOG(INFO) << "Finish building pyramidal for slice " << z << " of channel " << c;
        }
      }
    }
  }
  catch (H5::Exception const& e) {
    QFile::remove(filename);
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
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
