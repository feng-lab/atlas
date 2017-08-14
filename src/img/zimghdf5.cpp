#include "ZImgHDF5.h"

#include "zioutils.h"
#include "zimgsliceprovider.h"
#include "zlog.h"
#include "ZImgInfoIO.h"
#include <QFile>

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

void ZImgHDF5::writeImg(const QString& filename, const ZImg& img, Compression comp)
{
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_TRUNC);

    hsize_t imgDim[2] = {img.width(), img.height()};
    hsize_t chunkDim[2] = {std::min(img.width(), 512_usize), std::min(img.height(), 512_usize)};

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
    H5::StrType strType(0, H5T_VARIABLE);

    H5::DataSpace attrDataSpace(H5S_SCALAR);

    H5::Group allGrp = file.createGroup("Img");

    ZImgInfoIO::save(allGrp, img.info());

    H5::DataSpace imgDataspace(2, imgDim);
    H5::DSetCreatPropList pList;
    pList.setDeflate(9);
    pList.setChunk(2, chunkDim);

    for (size_t t = 0; t < img.numTimes(); ++t) {
      H5::Group timeGrp = allGrp.createGroup(qUtf8Printable(QString("Time%1").arg(t)));
      for (size_t c = 0; c < img.numChannels(); ++c) {
        H5::Group channelGrp = timeGrp.createGroup(qUtf8Printable(QString("Channel%1").arg(c)));
        for (size_t z = 0; z < img.depth(); ++z) {
          H5::Group zGrp = channelGrp.createGroup(qUtf8Printable(QString("Z%1").arg(z)));

          H5::DataSet imgData;

          if (img.voxelFormat() == VoxelFormat::Unsigned) {
            switch (img.bytesPerVoxel()) {
              case 1:
                imgData = zGrp.createDataSet("Data", uint8Type, imgDataspace, pList);
                imgData.write(img.planeData<uint8_t>(z, c, t), uint8Type);
                break;
              case 2:
                imgData = zGrp.createDataSet("Data", uint16Type, imgDataspace, pList);
                imgData.write(img.planeData<uint16_t>(z, c, t), uint16Type);
                break;
              case 4:
                imgData = zGrp.createDataSet("Data", uint32Type, imgDataspace, pList);
                imgData.write(img.planeData<uint32_t>(z, c, t), uint32Type);
                break;
              case 8:
                imgData = zGrp.createDataSet("Data", uint64Type, imgDataspace, pList);
                imgData.write(img.planeData<uint64_t>(z, c, t), uint64Type);
                break;
              default:
                break;
            }
          } else if (img.voxelFormat() == VoxelFormat::Float) {
            switch (img.bytesPerVoxel()) {
              case 4:
                imgData = zGrp.createDataSet("Data", floatType, imgDataspace, pList);
                imgData.write(img.planeData<float>(z, c, t), floatType);
                break;
              case 8:
                imgData = zGrp.createDataSet("Data", doubleType, imgDataspace, pList);
                imgData.write(img.planeData<double>(z, c, t), doubleType);
                break;
              default:
                break;
            }
          } else if (img.voxelFormat() == VoxelFormat::Signed) {
            switch (img.bytesPerVoxel()) {
              case 1:
                imgData = zGrp.createDataSet("Data", int8Type, imgDataspace, pList);
                imgData.write(img.planeData<int8_t>(z, c, t), int8Type);
                break;
              case 2:
                imgData = zGrp.createDataSet("Data", int16Type, imgDataspace, pList);
                imgData.write(img.planeData<int16_t>(z, c, t), int16Type);
                break;
              case 4:
                imgData = zGrp.createDataSet("Data", int32Type, imgDataspace, pList);
                imgData.write(img.planeData<int32_t>(z, c, t), int32Type);
                break;
              case 8:
                imgData = zGrp.createDataSet("Data", int64Type, imgDataspace, pList);
                imgData.write(img.planeData<int64_t>(z, c, t), int64Type);
                break;
              default:
                break;
            }
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

void ZImgHDF5::writeImg(const QString& filename, const ZImgSliceProvider& imgSliceProvider, Compression comp)
{
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_TRUNC);

    hsize_t imgDim[2] = {imgSliceProvider.imgInfo().width,
                         imgSliceProvider.imgInfo().height};
    hsize_t chunkDim[2] = {std::min(imgSliceProvider.imgInfo().width, 512_usize),
                           std::min(imgSliceProvider.imgInfo().height, 512_usize)};

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

    H5::DataSpace attrDataSpace(H5S_SCALAR);

    H5::Group allGrp = file.createGroup("Img");

    ZImgInfoIO::save(allGrp, imgSliceProvider.imgInfo());

    H5::DataSpace imgDataspace(2, imgDim);
    H5::DSetCreatPropList pList;
    pList.setDeflate(9);
    pList.setChunk(2, chunkDim);

    for (size_t t = 0; t < imgSliceProvider.imgInfo().numTimes; ++t) {
      H5::Group timeGrp = allGrp.createGroup(qUtf8Printable(QString("Time%1").arg(t)));
      for (size_t c = 0; c < imgSliceProvider.imgInfo().numChannels; ++c) {
        H5::Group channelGrp = timeGrp.createGroup(qUtf8Printable(QString("Channel%1").arg(c)));
        for (size_t z = 0; z < imgSliceProvider.imgInfo().depth; ++z) {
          H5::Group zGrp = channelGrp.createGroup(qUtf8Printable(QString("Z%1").arg(z)));

          H5::DataSet imgData;
          ZImg img = imgSliceProvider.slice(z, t, 1);

          if (img.voxelFormat() == VoxelFormat::Unsigned) {
            switch (img.bytesPerVoxel()) {
              case 1:
                imgData = zGrp.createDataSet("Data", uint8Type, imgDataspace, pList);
                imgData.write(img.channelData<uint8_t>(c), uint8Type);
                break;
              case 2:
                imgData = zGrp.createDataSet("Data", uint16Type, imgDataspace, pList);
                imgData.write(img.channelData<uint16_t>(c), uint16Type);
                break;
              case 4:
                imgData = zGrp.createDataSet("Data", uint32Type, imgDataspace, pList);
                imgData.write(img.channelData<uint32_t>(c), uint32Type);
                break;
              case 8:
                imgData = zGrp.createDataSet("Data", uint64Type, imgDataspace, pList);
                imgData.write(img.channelData<uint64_t>(c), uint64Type);
                break;
              default:
                break;
            }
          } else if (img.voxelFormat() == VoxelFormat::Float) {
            switch (img.bytesPerVoxel()) {
              case 4:
                imgData = zGrp.createDataSet("Data", floatType, imgDataspace, pList);
                imgData.write(img.channelData<float>(c), floatType);
                break;
              case 8:
                imgData = zGrp.createDataSet("Data", doubleType, imgDataspace, pList);
                imgData.write(img.channelData<double>(c), doubleType);
                break;
              default:
                break;
            }
          } else if (img.voxelFormat() == VoxelFormat::Signed) {
            switch (img.bytesPerVoxel()) {
              case 1:
                imgData = zGrp.createDataSet("Data", int8Type, imgDataspace, pList);
                imgData.write(img.channelData<int8_t>(c), int8Type);
                break;
              case 2:
                imgData = zGrp.createDataSet("Data", int16Type, imgDataspace, pList);
                imgData.write(img.channelData<int16_t>(c), int16Type);
                break;
              case 4:
                imgData = zGrp.createDataSet("Data", int32Type, imgDataspace, pList);
                imgData.write(img.channelData<int32_t>(c), int32Type);
                break;
              case 8:
                imgData = zGrp.createDataSet("Data", int64Type, imgDataspace, pList);
                imgData.write(img.channelData<int64_t>(c), int64Type);
                break;
              default:
                break;
            }
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
