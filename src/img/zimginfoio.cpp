#include "zimginfoio.h"

#include "zlog.h"

namespace nim {

ZImgInfo ZImgInfoIO::load(const H5::Group& grp)
{
  try {
    ZImgInfo info;

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

    H5::Attribute verattr = grp.openAttribute("Version");
    int32_t ver;
    verattr.read(int32Type, &ver);

    {
      H5::Attribute attr = grp.openAttribute("NumberOfTimePoints");
      uint64_t v;
      attr.read(uint64Type, &v);
      info.numTimes = v;
    }

    {
      H5::Attribute attr = grp.openAttribute("NumberOfChannels");
      uint64_t v;
      attr.read(uint64Type, &v);
      info.numChannels = v;
    }

    {
      H5::Attribute attr = grp.openAttribute("Width");
      uint64_t v;
      attr.read(uint64Type, &v);
      info.width = v;
    }

    {
      H5::Attribute attr = grp.openAttribute("Height");
      uint64_t v;
      attr.read(uint64Type, &v);
      info.height = v;
    }

    {
      H5::Attribute attr = grp.openAttribute("Depth");
      uint64_t v;
      attr.read(uint64Type, &v);
      info.depth = v;
    }

    {
      H5::Attribute attr = grp.openAttribute("DataType");
      H5std_string strBuf;
      attr.read(strType, strBuf);
      info.setVoxelFormat(QString::fromStdString(strBuf));
    }

    {
      H5::Attribute attr = grp.openAttribute("ValidBitCount");
      uint64_t v;
      attr.read(uint64Type, &v);
      info.validBitCount = v;
    }

    {
      H5::Attribute attr = grp.openAttribute("VoxelSizeUnit");
      H5std_string strBuf;
      attr.read(strType, strBuf);
      if (strBuf == "none") {
        info.voxelSizeUnit = VoxelSizeUnit::none;
      } else if (strBuf == "inch") {
        info.voxelSizeUnit = VoxelSizeUnit::inch;
      } else if (strBuf == "cm") {
        info.voxelSizeUnit = VoxelSizeUnit::cm;
      } else if (strBuf == "mm") {
        info.voxelSizeUnit = VoxelSizeUnit::mm;
      } else if (strBuf == "um") {
        info.voxelSizeUnit = VoxelSizeUnit::um;
      } else if (strBuf == "nm") {
        info.voxelSizeUnit = VoxelSizeUnit::nm;
      } else if (strBuf == "m") {
        info.voxelSizeUnit = VoxelSizeUnit::m;
      } else if (strBuf == "km") {
        info.voxelSizeUnit = VoxelSizeUnit::km;
      } else if (strBuf == "hm") {
        info.voxelSizeUnit = VoxelSizeUnit::hm;
      } else {
        throw ZIOException(fmt::format("invalid voxel size unit {}", strBuf));
      }
    }

    {
      H5::Attribute attr = grp.openAttribute("VoxelSizeX");
      attr.read(doubleType, &info.voxelSizeX);
    }

    {
      H5::Attribute attr = grp.openAttribute("VoxelSizeY");
      attr.read(doubleType, &info.voxelSizeY);
    }

    {
      H5::Attribute attr = grp.openAttribute("VoxelSizeZ");
      attr.read(doubleType, &info.voxelSizeZ);
    }

    {
      H5::Attribute attr = grp.openAttribute("TimeStamps");
      if (attr.getSpace().getSimpleExtentNdims() != 1) {
        throw ZIOException("wrong TimeStamps dimension number");
      }
      hsize_t dims[1];
      attr.getSpace().getSimpleExtentDims(dims);
      if (dims[0] != info.numTimes) {
        LOG(WARNING) << "TimeStamps dimension does not match image dimension";
      }
      if (dims[0] > 0) {
        info.timeStamps.resize(dims[0]);
        attr.read(doubleType, info.timeStamps.data());
      }
    }

    {
      H5::Attribute attr = grp.openAttribute("ChannelNames");
      if (attr.getSpace().getSimpleExtentNdims() != 1) {
        throw ZIOException("wrong ChannelNames dimension number");
      }
      hsize_t dims[1];
      attr.getSpace().getSimpleExtentDims(dims);
      if (dims[0] != info.numChannels) {
        LOG(WARNING) << "ChannelNames dimension does not match image dimension";
      }
      if (dims[0] > 0) {
        std::vector<char*> cStrArray(dims[0]);
        attr.read(strType, cStrArray.data());
        for (auto p : cStrArray) {
          info.channelNames.emplace_back(p);
          delete[] p;
        }
      }
    }

    {
      H5::Attribute attr = grp.openAttribute("ChannelColors");
      if (attr.getSpace().getSimpleExtentNdims() != 2) {
        throw ZIOException("wrong ChannelColors dimension number");
      }
      hsize_t dims[2];
      attr.getSpace().getSimpleExtentDims(dims);
      if (dims[1] != 4) {
        throw ZIOException("wrong ChannelColors dimension");
      }
      if (dims[0] != info.numChannels) {
        LOG(WARNING) << "ChannelColors dimension does not match image dimension";
      }
      if (dims[0] > 0) {
        info.channelColors.resize(dims[0]);
        attr.read(uint8Type, &info.channelColors[0].r);
      }
    }

    {
      H5::Attribute attr = grp.openAttribute("Position");
      if (attr.getSpace().getSimpleExtentNdims() != 1) {
        throw ZIOException("wrong Position dimension number");
      }
      hsize_t dims[1];
      attr.getSpace().getSimpleExtentDims(dims);
      if (dims[0] > 0) {
        info.position.resize(dims[0]);
        attr.read(doubleType, info.position.data());
      }
    }

    {
      H5::Attribute attr = grp.openAttribute("LastChannelIsAlphaChannel");
      int32_t v;
      attr.read(int32Type, &v);
      info.lastChannelIsAlphaChannel = v;
    }

    info.createDefaultDescriptions();

    return info;
  }
  catch (H5::Exception const& e) {
    throw ZIOException(fmt::format("hdf5:{}", e.getDetailMsg()));
  }
}

void ZImgInfoIO::save(H5::Group& grp, const ZImgInfo& info)
{
  try {
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

    H5::Attribute ver = grp.createAttribute("Version", int32Type, attrDataSpace);
    int32_t h5ImagVer = 100;
    ver.write(int32Type, &h5ImagVer);

    uint64_t nt = info.numTimes;
    H5::Attribute numTimesAttr = grp.createAttribute("NumberOfTimePoints", uint64Type, attrDataSpace);
    numTimesAttr.write(uint64Type, &nt);

    uint64_t nc = info.numChannels;
    H5::Attribute numChannelsAttr = grp.createAttribute("NumberOfChannels", uint64Type, attrDataSpace);
    numChannelsAttr.write(uint64Type, &nc);

    uint64_t w = info.width;
    H5::Attribute widthAttr = grp.createAttribute("Width", uint64Type, attrDataSpace);
    widthAttr.write(uint64Type, &w);

    uint64_t h = info.height;
    H5::Attribute heightAttr = grp.createAttribute("Height", uint64Type, attrDataSpace);
    heightAttr.write(uint64Type, &h);

    uint64_t d = info.depth;
    H5::Attribute depthAttr = grp.createAttribute("Depth", uint64Type, attrDataSpace);
    depthAttr.write(uint64Type, &d);

    H5::Attribute dt = grp.createAttribute("DataType", strType, attrDataSpace);
    dt.write(strType, info.typeAsQString().toStdString());

    uint64_t vbc = info.validBitCount;
    H5::Attribute vbcAttr = grp.createAttribute("ValidBitCount", uint64Type, attrDataSpace);
    vbcAttr.write(uint64Type, &vbc);

    H5::Attribute vsu = grp.createAttribute("VoxelSizeUnit", strType, attrDataSpace);
    vsu.write(strType, std::string(enumToString(info.voxelSizeUnit)));

    H5::Attribute vsxAttr = grp.createAttribute("VoxelSizeX", doubleType, attrDataSpace);
    vsxAttr.write(doubleType, &info.voxelSizeX);

    H5::Attribute vsyAttr = grp.createAttribute("VoxelSizeY", doubleType, attrDataSpace);
    vsyAttr.write(doubleType, &info.voxelSizeY);

    H5::Attribute vszAttr = grp.createAttribute("VoxelSizeZ", doubleType, attrDataSpace);
    vszAttr.write(doubleType, &info.voxelSizeZ);

    CHECK(info.timeStamps.size() == info.numTimes);
    CHECK(info.channelNames.size() == info.numChannels);
    CHECK(info.channelColors.size() == info.numChannels);

    {
      hsize_t dims[1] = {info.numTimes};
      H5::DataSpace ds(1, dims);
      H5::Attribute attr = grp.createAttribute("TimeStamps", doubleType, ds);
      if (info.numTimes > 0) {
        attr.write(doubleType, info.timeStamps.data());
      }
    }

    {
      hsize_t dims[1] = {info.numChannels};
      H5::DataSpace ds(1, dims);
      H5::Attribute attr = grp.createAttribute("ChannelNames", strType, ds);
      if (info.numChannels > 0) {
        std::vector<std::string> sa;
        std::vector<const char*> cStrArray;
        for (const auto& str : info.channelNames) {
          sa.push_back(str.toStdString());
        }
        cStrArray.reserve(sa.size());
        for (const auto& str : sa) {
          cStrArray.push_back(str.c_str());
        }
        attr.write(strType, cStrArray.data());
      }
    }

    {
      hsize_t dims[2] = {info.numChannels, 4};
      H5::DataSpace ds(2, dims);
      H5::Attribute attr = grp.createAttribute("ChannelColors", uint8Type, ds);
      if (info.numChannels > 0) {
        attr.write(uint8Type, &info.channelColors[0].r);
      }
    }

    {
      hsize_t dims[1] = {info.position.size()};
      H5::DataSpace ds(1, dims);
      H5::Attribute attr = grp.createAttribute("Position", doubleType, ds);
      if (!info.position.empty()) {
        attr.write(doubleType, info.position.data());
      }
    }

    int32_t lastChannelIsAlphaChannel = info.lastChannelIsAlphaChannel;
    H5::Attribute lciacAttr = grp.createAttribute("LastChannelIsAlphaChannel", int32Type, attrDataSpace);
    lciacAttr.write(int32Type, &lastChannelIsAlphaChannel);
  }
  catch (H5::Exception const& e) {
    throw ZIOException(fmt::format("hdf5:{}", e.getDetailMsg()));
  }
}

} // namespace nim
