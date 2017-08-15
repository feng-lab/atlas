#include "zimghdf5.h"

#include "zioutils.h"
#include "zimgsliceprovider.h"
#include "zlog.h"
#include "zimginfoio.h"
#include <QFile>
#include <QMutexLocker>

namespace {

inline size_t chunkSize()
{
  return size_t(512);
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

  if (dims[0] < img.width() + x_ || dims[1] < img.height() + y_)
    throw nim::ZIOException("wrong slice data dimension");

  hsize_t offset[2] = { x_, y_ };
  hsize_t count[2] = { img.width(), img.height() };
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

void writeImgSliceToH5Grp(H5::Group& zGrp, const H5std_string& name, const nim::ZImg& img)
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

  hsize_t imgDim[2] = {img.width(), img.height()};
  hsize_t chunkDim[2] = {std::min(img.width(), chunkSize()), std::min(img.height(), chunkSize())};
  H5::DataSpace imgDataspace(2, imgDim);
  H5::DSetCreatPropList pList;
  pList.setDeflate(9);
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

std::set<size_t> loadRatiosFromH5Grp(const H5::Group& grp)
{
  H5::IntType uint64Type(H5::PredType::STD_U64LE);
  uint64_t numLevels;
  std::vector<uint64_t> levels;

  {
    H5::Attribute attr = grp.openAttribute("NumberOfLevels");
    attr.read(uint64Type, &numLevels);
  }

  {
    H5::Attribute attr = grp.openAttribute("Levels");
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
    H5::Attribute attr = grp.createAttribute("NumberOfLevels", uint64Type, ds);
    attr.write(uint64Type, &numLevels);
  }

  {
    hsize_t dims[1] = {levels.size()};
    H5::DataSpace ds(1, dims);
    H5::Attribute attr = grp.createAttribute("Levels", uint64Type, ds);
    attr.write(uint64Type, levels.data());
  }
}

}

namespace nim {

ZImgHDF5SubBlock::ZImgHDF5SubBlock(const QString& fileName, const ZImgInfo& info,
                                   size_t ratio_, size_t t_, size_t z_, size_t x_, size_t y_)
  : ZImgSubBlock(ratio_, t_, z_, x_ * ratio_, y_ * ratio_, info.width * ratio_, info.height * ratio_)
  , m_filename(fileName)
  , m_info(info)
  , m_ratio(ratio_)
  , m_x(x_)
  , m_y(y_)
{
  for (size_t c = 0; c < info.numChannels; ++c) {
    if (m_ratio == 1) {
      m_tiles.push_back(QString("/Img/Time%1/Channel%2/Z%3/Data").arg(t_).arg(c).arg(z_).toStdString());
    } else {
      m_tiles.push_back(
        QString("/Img/Time%1/Channel%2/Z%3/Level%4Data").arg(t_).arg(c).arg(z_).arg(ratio).toStdString());
    }
  }
}

std::shared_ptr<ZImg> ZImgHDF5SubBlock::read() const
{
  // todo: fix hdf5 multithread reading
  static QMutex lock;
  QMutexLocker lk(&lock);
  try {
    if (m_tiles.empty()) {
      throw ZIOException("empty hdf5 sub block");
    }
    auto res = std::make_shared<ZImg>();

    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(m_filename).constData(), H5F_ACC_RDONLY);

    if (m_tiles.size() == 1) {
      LOG(INFO) << m_tiles[0] << m_info.toQString();
      H5::DataSet ds = file.openDataSet(m_tiles[0]);
      *res = ZImg(m_info);
      readH5DataToImg(*res, ds, m_x, m_y);
    } else {
      std::vector<ZImg> imgs(m_tiles.size());
      for (size_t i = 0; i < m_tiles.size(); ++i) {
        H5::DataSet ds = file.openDataSet(m_tiles[i]);
        imgs[i] = ZImg(m_info);
        readH5DataToImg(imgs[i], ds, m_x, m_y);
      }
      *res = ZImg::cat(imgs, Dimension::C);
    }

    return res;
  }
  catch (H5::Exception const& e) {
    throw ZIOException(QString("read %1 hdf5:%2").arg(m_filename).arg(e.getDetailMsg().c_str()));
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
  res << "h5";
  return res;
}

void ZImgHDF5::readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                          std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                          std::vector<std::set<size_t>>* pyramidalRatios)
{
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_RDONLY);

    H5::Group allGrp = file.openGroup("Img");

    infos.resize(1);
    infos[0] = ZImgInfoIO::load(allGrp);

    //createDefaultSubBlocks(filename, infos, subBlocks, pyramidalRatios);

    std::set<size_t> levels = loadRatiosFromH5Grp(allGrp);

    if (subBlocks) {
      subBlocks->resize(infos.size());
      auto& subBlock = subBlocks->at(0);
      if (!infos[0].isEmpty()) {
        H5::DataSet ds = allGrp.openDataSet("Time0/Channel0/Z0/Data");
        H5::DSetCreatPropList pList = ds.getCreatePlist();
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
                  subBlock.emplace_back(
                    std::make_shared<ZImgHDF5SubBlock>(filename, inf, level, t, z, x, y));
                }
              }
            }
          }
        }
      }
    }
    if (pyramidalRatios) {
      pyramidalRatios->resize(infos.size());
      pyramidalRatios->at(0) = levels;
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

void ZImgHDF5::readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene, size_t ratio)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_RDONLY);

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

    CHECK(ratio >= 1);
    size_t readRatio = 0;
    for (auto r : pyRatios) {
      if (r <= ratio) {
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
      readRatio == 1 ? std::string("Data") : QString("Level%1Data").arg(readRatio).toStdString();

    for (size_t t = 0; t < info.numTimes; ++t) {
      if (!rgn.tInRegion(t)) {
        continue;
      }
      H5::Group timeGrp = allGrp.openGroup(qUtf8Printable(QString("Time%1").arg(t)));
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

    if (ratio > readRatio) {
      img.zoom(1.0 * readRatio / ratio, 1.0 * readRatio / ratio);
    }
  }
  catch (H5::Exception const& e) {
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
  }
}

void ZImgHDF5::writeImg(const QString& filename, const ZImg& img, Compression)
{
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_TRUNC);

    H5::Group allGrp = file.createGroup("Img");

    ZImgInfoIO::save(allGrp, img.info());

    uint64_t numLevels = 1;
    std::set<size_t> levels{1};
    size_t level = 1;
    size_t width = img.width();
    size_t height = img.height();
    while (width > chunkSize() && height > chunkSize()) {
      ++numLevels;
      level *= 2;
      levels.insert(level);
      width = std::ceil(width / 2.0);
      height = std::ceil(height / 2.0);
    }

    writeRatiosToGrp(allGrp, levels);

    for (size_t t = 0; t < img.numTimes(); ++t) {
      H5::Group timeGrp = allGrp.createGroup(qUtf8Printable(QString("Time%1").arg(t)));
      for (size_t c = 0; c < img.numChannels(); ++c) {
        H5::Group channelGrp = timeGrp.createGroup(qUtf8Printable(QString("Channel%1").arg(c)));
        for (size_t z = 0; z < img.depth(); ++z) {
          H5::Group zGrp = channelGrp.createGroup(qUtf8Printable(QString("Z%1").arg(z)));

          ZImg tmpImg = img.createView(z, c, t);
          writeImgSliceToH5Grp(zGrp, "Data", tmpImg);

          level = 1;
          while (tmpImg.width() > chunkSize() && tmpImg.height() > chunkSize()) {
            level *= 2;
            tmpImg.zoom(0.5, 0.5);
            writeImgSliceToH5Grp(zGrp, QString("Level%1Data").arg(level).toStdString(), tmpImg);
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

void ZImgHDF5::writeImg(const QString& filename, const ZImgSliceProvider& imgSliceProvider, Compression)
{
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_TRUNC);

    H5::IntType uint64Type(H5::PredType::STD_U64LE);

    H5::Group allGrp = file.createGroup("Img");

    ZImgInfoIO::save(allGrp, imgSliceProvider.imgInfo());

    std::set<size_t> levels = imgSliceProvider.ratios();
    writeRatiosToGrp(allGrp, levels);

    for (size_t t = 0; t < imgSliceProvider.imgInfo().numTimes; ++t) {
      H5::Group timeGrp = allGrp.createGroup(qUtf8Printable(QString("Time%1").arg(t)));
      for (size_t c = 0; c < imgSliceProvider.imgInfo().numChannels; ++c) {
        H5::Group channelGrp = timeGrp.createGroup(qUtf8Printable(QString("Channel%1").arg(c)));
        for (size_t z = 0; z < imgSliceProvider.imgInfo().depth; ++z) {
          H5::Group zGrp = channelGrp.createGroup(qUtf8Printable(QString("Z%1").arg(z)));

          writeImgSliceToH5Grp(zGrp, "Data", imgSliceProvider.slice(z, t, 1).createView(c, 0));

          for (auto level : levels) {
            if (level == 1)
              continue;
            writeImgSliceToH5Grp(zGrp, QString("Level%1Data").arg(level).toStdString(),
                                 imgSliceProvider.slice(z, t, level).createView(c, 0));
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

bool ZImgHDF5::supportRead() const
{
  return true;
}

bool ZImgHDF5::supportWrite() const
{
  return true;
}

} // namespace nim
