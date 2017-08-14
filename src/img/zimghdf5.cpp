#include "zimghdf5.h"

#include "zioutils.h"
#include "zimgsliceprovider.h"
#include "zlog.h"
#include "zimginfoio.h"
#include <QFile>

namespace {

inline size_t chunkSize()
{
  return size_t(512);
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

}

namespace nim {

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

    createDefaultSubBlocks(filename, infos, subBlocks, pyramidalRatios);
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

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_RDONLY);

    H5::Group allGrp = file.openGroup("Img");

    ZImgInfo imgInfo = ZImgInfoIO::load(allGrp);

    if (region.isEmpty() || !region.isValid(imgInfo)) {
      throw ZIOException(
        QString("Invalid image region. Image info: '%1', region: '%2'").arg(imgInfo.toQString()).arg(
          region.toQString()));
    }

    ZImgInfo imgInfo2D(imgInfo);
    imgInfo2D.depth = 1;
    imgInfo2D.numTimes = 1;
    imgInfo2D.numChannels = 1;
    ZImg buf2DImg(imgInfo2D);

    ZImgInfo partialImgInfo = region.clip(imgInfo);
    ZImg imgTmp(partialImgInfo);

    for (size_t t = 0; t < imgInfo.numTimes; ++t) {
      if (!region.tInRegion(t)) {
        continue;
      }
      H5::Group timeGrp = allGrp.openGroup(qUtf8Printable(QString("Time%1").arg(t)));
      for (size_t c = 0; c < imgInfo.numChannels; ++c) {
        if (!region.cInRegion(c)) {
          continue;
        }
        H5::Group channelGrp = timeGrp.openGroup(qUtf8Printable(QString("Channel%1").arg(c)));
        for (size_t z = 0; z < imgInfo.depth; ++z) {
          if (!region.zInRegion(z)) {
            continue;
          }
          H5::Group zGrp = channelGrp.openGroup(qUtf8Printable(QString("Z%1").arg(z)));

          H5::DataSet data = zGrp.openDataSet("Data");
          H5::DataSpace dataDataspace = data.getSpace();

          if (data.getSpace().getSimpleExtentNdims() != 2)
            throw ZIOException("wrong slice data dimension number");

          hsize_t dims[2];
          data.getSpace().getSimpleExtentDims(dims);

          if (dims[0] != imgInfo.width || dims[1] != imgInfo.height)
            throw ZIOException("wrong slice data dimension");

          if (imgInfo.voxelFormat == VoxelFormat::Unsigned) {
            switch (imgInfo.bytesPerVoxel) {
              case 1:
                data.read(buf2DImg.timeData(0), uint8Type);
                break;
              case 2:
                data.read(buf2DImg.timeData(0), uint16Type);
                break;
              case 4:
                data.read(buf2DImg.timeData(0), uint32Type);
                break;
              case 8:
                data.read(buf2DImg.timeData(0), uint64Type);
                break;
              default:
                break;
            }
          } else if (imgInfo.voxelFormat == VoxelFormat::Float) {
            switch (imgInfo.bytesPerVoxel) {
              case 4:
                data.read(buf2DImg.timeData(0), floatType);
                break;
              case 8:
                data.read(buf2DImg.timeData(0), doubleType);
                break;
              default:
                break;
            }
          } else if (imgInfo.voxelFormat == VoxelFormat::Signed) {
            switch (imgInfo.bytesPerVoxel) {
              case 1:
                data.read(buf2DImg.timeData(0), int8Type);
                break;
              case 2:
                data.read(buf2DImg.timeData(0), int16Type);
                break;
              case 4:
                data.read(buf2DImg.timeData(0), int32Type);
                break;
              case 8:
                data.read(buf2DImg.timeData(0), int64Type);
                break;
              default:
                break;
            }
          }

          if (region.containsWholeRow(buf2DImg.info())) {
            memcpy(imgTmp.planeData<uint8_t>(z - region.start.z, c - region.start.c, t - region.start.t),
                   buf2DImg.rowData<uint8_t>(region.start.y, 0, 0, 0),
                   imgTmp.planeByteNumber());
          } else {
            size_t yEnd = region.end.y == -1 ? buf2DImg.height() : region.end.y;
            for (size_t y = region.start.y; y < yEnd; ++y) {
              memcpy(imgTmp.rowData<uint8_t>(y - region.start.y, z - region.start.z,
                                             c - region.start.c, t - region.start.t),
                     buf2DImg.data<uint8_t>(region.start.x, y, 0, 0, 0),
                     imgTmp.rowByteNumber());
            }
          }
        }
      }
    }

    img.swap(imgTmp);

    if (ratio > 1) {
      img.zoom(1.0 / ratio, 1.0 / ratio);
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

    H5::IntType uint64Type(H5::PredType::STD_U64LE);

    H5::Group allGrp = file.createGroup("Img");

    ZImgInfoIO::save(allGrp, img.info());

    uint64_t numLevels = 1;
    size_t width = img.width();
    size_t height = img.height();
    while (width > chunkSize() && height > chunkSize()) {
      ++numLevels;
      width = std::ceil(width / 2.0);
      height = std::ceil(height / 2.0);
    }
    std::vector<uint64_t> levels{1};
    for (uint64_t l = 1; l < numLevels; ++l) {
      levels.push_back((*levels.rbegin()) * 2);
    }

    {
      H5::DataSpace ds(H5S_SCALAR);
      H5::Attribute attr = allGrp.createAttribute("NumberOfLevels", uint64Type, ds);
      attr.write(uint64Type, &numLevels);
    }

    {
      hsize_t dims[1] = {levels.size()};
      H5::DataSpace ds(1, dims);
      H5::Attribute attr = allGrp.createAttribute("Levels", uint64Type, ds);
      attr.write(uint64Type, levels.data());
    }

    for (size_t t = 0; t < img.numTimes(); ++t) {
      H5::Group timeGrp = allGrp.createGroup(qUtf8Printable(QString("Time%1").arg(t)));
      for (size_t c = 0; c < img.numChannels(); ++c) {
        H5::Group channelGrp = timeGrp.createGroup(qUtf8Printable(QString("Channel%1").arg(c)));
        for (size_t z = 0; z < img.depth(); ++z) {
          H5::Group zGrp = channelGrp.createGroup(qUtf8Printable(QString("Z%1").arg(z)));

          ZImg tmpImg = img.createView(z, c, t);
          writeImgSliceToH5Grp(zGrp, "Data", tmpImg);

          uint64_t level = 1;
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

    std::vector<uint64_t> levels;
    auto ratios = imgSliceProvider.ratios();
    levels.insert(levels.end(), ratios.begin(), ratios.end());
    uint64_t numLevels = levels.size();

    {
      H5::DataSpace ds(H5S_SCALAR);
      H5::Attribute attr = allGrp.createAttribute("NumberOfLevels", uint64Type, ds);
      attr.write(uint64Type, &numLevels);
    }

    {
      hsize_t dims[1] = {levels.size()};
      H5::DataSpace ds(1, dims);
      H5::Attribute attr = allGrp.createAttribute("Levels", uint64Type, ds);
      attr.write(uint64Type, levels.data());
    }

    for (size_t t = 0; t < imgSliceProvider.imgInfo().numTimes; ++t) {
      H5::Group timeGrp = allGrp.createGroup(qUtf8Printable(QString("Time%1").arg(t)));
      for (size_t c = 0; c < imgSliceProvider.imgInfo().numChannels; ++c) {
        H5::Group channelGrp = timeGrp.createGroup(qUtf8Printable(QString("Channel%1").arg(c)));
        for (size_t z = 0; z < imgSliceProvider.imgInfo().depth; ++z) {
          H5::Group zGrp = channelGrp.createGroup(qUtf8Printable(QString("Z%1").arg(z)));

          writeImgSliceToH5Grp(zGrp, "Data", imgSliceProvider.slice(z, t, 1).createView(c, 0));

          for (size_t lidx = 1; lidx < levels.size(); ++lidx) {
            writeImgSliceToH5Grp(zGrp, QString("Level%1Data").arg(levels[lidx]).toStdString(),
                                 imgSliceProvider.slice(z, t, levels[lidx]).createView(c, 0));
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
