#include "../version/version.h"
#include "typecast.h"
#include "zimg.h"
#include "zpuncta.h"
#include "zimginit.h"
#include "zstitchimage.h"
#include "zimgnccmatch.h"
#include "zimgblockprovider.h"
#include "zimgsliceprovider.h"
#include "zimgmerge.h"
#include "zpunctadetection.h"
#include "zsectionsregistration.h"
#include "zchromaticshiftcorrection.h"
#include "zroiutils.h"
#include "zimgautothreshold.h"
#include "zswc.h"
#include "zmesh.h"
#include <fmt/core.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include <pybind11/eigen.h>
#include <pybind11/operators.h>

namespace py = pybind11;

using namespace nim;

using namespace pybind11::literals;

namespace {

std::string getFormatDesc(const ZImg& img)
{
  if (img.voxelFormat() == VoxelFormat::Unsigned) {
    switch (img.bytesPerVoxel()) {
      case 1:
        return py::format_descriptor<uint8_t>::format();
      case 2:
        return py::format_descriptor<uint16_t>::format();
      case 4:
        return py::format_descriptor<uint32_t>::format();
      case 8:
        return py::format_descriptor<uint64_t>::format();
      default:
        throw ZException("Incorrect Img Info");
    }
  } else if (img.voxelFormat() == VoxelFormat::Float) {
    switch (img.bytesPerVoxel()) {
      case 4:
        return py::format_descriptor<float>::format();
      case 8:
        return py::format_descriptor<double>::format();
      default:
        throw ZException("Incorrect Img Info");
    }
  } else {
    switch (img.bytesPerVoxel()) {
      case 1:
        return py::format_descriptor<int8_t>::format();
      case 2:
        return py::format_descriptor<int16_t>::format();
      case 4:
        return py::format_descriptor<int32_t>::format();
      case 8:
        return py::format_descriptor<int64_t>::format();
      default:
        throw ZException("Incorrect Img Info");
    }
  }
}

py::dtype getDType(const ZImg& img)
{
  if (img.voxelFormat() == VoxelFormat::Unsigned) {
    switch (img.bytesPerVoxel()) {
      case 1:
        return py::dtype::of<uint8_t>();
      case 2:
        return py::dtype::of<uint16_t>();
      case 4:
        return py::dtype::of<uint32_t>();
      case 8:
        return py::dtype::of<uint64_t>();
      default:
        throw ZException("Incorrect Img Info");
    }
  } else if (img.voxelFormat() == VoxelFormat::Float) {
    switch (img.bytesPerVoxel()) {
      case 4:
        return py::dtype::of<float>();
      case 8:
        return py::dtype::of<double>();
      default:
        throw ZException("Incorrect Img Info");
    }
  } else {
    switch (img.bytesPerVoxel()) {
      case 1:
        return py::dtype::of<int8_t>();
      case 2:
        return py::dtype::of<int16_t>();
      case 4:
        return py::dtype::of<int32_t>();
      case 8:
        return py::dtype::of<int64_t>();
      default:
        throw ZException("Incorrect Img Info");
    }
  }
}

ZImgInfo getImgInfoFromNdarray(const py::array& arr, const ZImgInfo& info_in)
{
  if (arr.ndim() != 4) { throw ZException("Only support 4d array: channel x depth x height x width"); }
  if (arr.size() <= 0) { throw ZException("Empty ndarray"); }
  ZImgInfo res = info_in;
  res.numTimes = 1;
  res.numChannels = arr.shape(0);
  res.depth = arr.shape(1);
  res.height = arr.shape(2);
  res.width = arr.shape(3);
  res.bytesPerVoxel = arr.itemsize();

  if (res.numChannels > 1 && static_cast<py::ssize_t>(res.channelByteNumber()) != arr.strides(0)) {
    throw ZException("ndarray is not C_CONTIGUOUS");
  }
  if (res.depth > 1 && static_cast<py::ssize_t>(res.planeByteNumber()) != arr.strides(1)) {
    throw ZException("ndarray is not C_CONTIGUOUS");
  }
  if (res.height > 1 && static_cast<py::ssize_t>(res.rowByteNumber()) != arr.strides(2)) {
    throw ZException("ndarray is not C_CONTIGUOUS");
  }
  if (res.width > 1 && static_cast<py::ssize_t>(res.voxelByteNumber()) != arr.strides(3)) {
    throw ZException("ndarray is not C_CONTIGUOUS");
  }

  switch (arr.dtype().kind()) {
    case 'i':
      res.voxelFormat = VoxelFormat::Signed;
      break;
    case 'u':
      res.voxelFormat = VoxelFormat::Unsigned;
      break;
    case 'f':
      res.voxelFormat = VoxelFormat::Float;
      break;
    default:
      throw ZException("ndarray dtype is not supported");
  }
  res.createDefaultDescriptions();

  return res;
}

template<class ZImgSubBlockBase = ZImgSubBlock>
class PyZImgSubBlock : public ZImgSubBlockBase
{
public:
  using ZImgSubBlockBase::ZImgSubBlockBase; // Inherit constructors
  std::shared_ptr<ZImg> read() const override
  {
    PYBIND11_OVERRIDE_PURE(std::shared_ptr<ZImg>, ZImgSubBlockBase, read, );
  }
  ZImgInfo readInfo() const override { PYBIND11_OVERRIDE_PURE(ZImgInfo, ZImgSubBlockBase, readInfo, ); }
};
template<class ZImgTileSubBlockBase = ZImgTileSubBlock>
class PyZImgTileSubBlock : public PyZImgSubBlock<ZImgTileSubBlockBase>
{
public:
  using PyZImgSubBlock<ZImgTileSubBlockBase>::PyZImgSubBlock; // Inherit constructors (via PyA_Tpl's inherited constructors)
  std::shared_ptr<ZImg> read() const override
  {
    PYBIND11_OVERRIDE(std::shared_ptr<ZImg>, ZImgTileSubBlockBase, read, );
  }
  ZImgInfo readInfo() const override { PYBIND11_OVERRIDE(ZImgInfo, ZImgTileSubBlockBase, readInfo, ); }
};

class PyZImgSliceProvider : public ZImgSliceProvider {
public:
  /* Inherit the constructors */
  using ZImgSliceProvider::ZImgSliceProvider;

  ZImgInfo imgInfo() const override
  {
    PYBIND11_OVERRIDE_PURE(ZImgInfo, ZImgSliceProvider, imgInfo, );
  }

  ZImg slice(size_t z, size_t t) const override
  {
    PYBIND11_OVERRIDE_PURE(ZImg, ZImgSliceProvider, slice, z, t);
  }

  ZImg allSlices(size_t t) const override
  {
    PYBIND11_OVERRIDE(ZImg, ZImgSliceProvider, allSlices, t);
  }

  ZImg wholeImg() const override
  {
    PYBIND11_OVERRIDE(ZImg, ZImgSliceProvider, wholeImg, );
  }
};

class PyZImgBlockProvider : public ZImgBlockProvider {
public:
  /* Inherit the constructors */
  using ZImgBlockProvider::ZImgBlockProvider;

  ZImgInfo imgInfo() const override
  {
    PYBIND11_OVERRIDE_PURE(ZImgInfo, ZImgBlockProvider, imgInfo, );
  }

  size_t numBlocks() const override
  {
    PYBIND11_OVERRIDE_PURE(size_t, ZImgBlockProvider, numBlocks, );
  }

  ZImg block(size_t blockIdx) const override
  {
    PYBIND11_OVERRIDE_PURE(ZImg, ZImgBlockProvider, block, blockIdx);
  }

  ZVoxelCoordinate blockCoord(size_t blockIdx) const override
  {
    PYBIND11_OVERRIDE_PURE(ZVoxelCoordinate, ZImgBlockProvider, blockCoord, blockIdx);
  }

  ZImg wholeImg() const override
  {
    PYBIND11_OVERRIDE(ZImg, ZImgBlockProvider, wholeImg, );
  }
};

template<size_t L, typename T>
std::vector<glm::vec<L, T>> arrayToVecVec(const py::array_t<T, py::array::c_style | py::array::forcecast>& array)
{
  if (array.ndim() != 2 || array.shape(1) != L) {
    throw ZException(fmt::format("input array shape does not match output glm::vec{}", L));
  }
  std::vector<glm::vec<L, T>> res;
  if (array.shape(0) > 0) {
    res.resize(array.shape(0));
    std::memcpy(res.data(), array.data(), sizeof(T) * L * res.size());
  }
  return res;
}

template<size_t L, typename T>
py::array_t<T, py::array::c_style | py::array::forcecast> vecVecToArray(const std::vector<glm::vec<L, T>>& v)
{
  return v.empty() ? py::array_t<T, py::array::c_style | py::array::forcecast>() :
    py::array_t<T, py::array::c_style | py::array::forcecast>({v.size(), L}, &v[0].x);
}

template<typename T>
std::vector<T> arrayToVector(const py::array_t<T, py::array::c_style | py::array::forcecast>& array)
{
  if (array.ndim() != 1) {
    throw ZException(fmt::format("input array shape does not match output"));
  }
  std::vector<T> res;
  if (array.shape(0) > 0) {
    res.resize(array.shape(0));
    std::memcpy(res.data(), array.data(), sizeof(T) * res.size());
  }
  return res;
}

template<typename T>
py::array_t<T, py::array::c_style | py::array::forcecast> vectorToArray(const std::vector<T>& v)
{
  return v.empty() ? py::array_t<T, py::array::c_style | py::array::forcecast>() :
         py::array_t<T, py::array::c_style | py::array::forcecast>(v.size(), v.data());
}

}

PYBIND11_MODULE(_imgpy, m)
{
  initImgLib("_imgpy",
             qgetenv("Resources_DIR"),
             "", qgetenv("ZIMG_JARS_DIR"), "",
             false);

  m.doc() = R"pbdoc(
        Python interface to img lib.
    )pbdoc";

  py::enum_<VoxelFormat>(m, "VoxelFormat", py::arithmetic())
    .value("Unsigned", VoxelFormat::Unsigned)
    .value("Signed", VoxelFormat::Signed)
    .value("Float", VoxelFormat::Float);

  py::enum_<VoxelSizeUnit>(m, "VoxelSizeUnit", py::arithmetic())
    .value("none", VoxelSizeUnit::none)
    .value("inch", VoxelSizeUnit::inch)
    .value("cm", VoxelSizeUnit::cm)
    .value("mm", VoxelSizeUnit::mm)
    .value("um", VoxelSizeUnit::um)
    .value("nm", VoxelSizeUnit::nm)
    .value("m", VoxelSizeUnit::m)
    .value("hm", VoxelSizeUnit::hm)
    .value("km", VoxelSizeUnit::km);

  py::enum_<Interpolant>(m, "Interpolant", py::arithmetic())
    .value("Nearest", Interpolant::Nearest)
    .value("Linear", Interpolant::Linear)
    .value("Cubic", Interpolant::Cubic)
    .value("Lanczos2", Interpolant::Lanczos2)
    .value("Lanczos3", Interpolant::Lanczos3);

  py::enum_<Dimension>(m, "Dimension", py::arithmetic())
    .value("X", Dimension::X)
    .value("Y", Dimension::Y)
    .value("Z", Dimension::Z)
    .value("C", Dimension::C)
    .value("T", Dimension::T);

  py::enum_<ImgMergeMode>(m, "ImgMergeMode")
    .value("Max", ImgMergeMode::Max)
    .value("Min", ImgMergeMode::Min)
    .value("Mean", ImgMergeMode::Mean)
    .value("Median", ImgMergeMode::Median)
    .value("First", ImgMergeMode::First)
    .value("Interpolation", ImgMergeMode::Interpolation);

  py::enum_<FileFormat>(m, "FileFormat", py::arithmetic())
    .value("Unknown", FileFormat::Unknown)
    .value("HDF5Img", FileFormat::HDF5Img)
    .value("OmeTiff", FileFormat::OmeTiff)
    .value("Tiff", FileFormat::Tiff)
    .value("Vaa3DRaw", FileFormat::Vaa3DRaw)
    .value("ZeissLsm", FileFormat::ZeissLsm)
    .value("Jpeg", FileFormat::Jpeg)
    .value("JpegXR", FileFormat::JpegXR)
    .value("Png", FileFormat::Png)
    .value("FreeImage", FileFormat::FreeImage)
    .value("MetaImage", FileFormat::MetaImage)
    .value("ZeissCZI", FileFormat::ZeissCZI)
    .value("ITKImage", FileFormat::ITKImage)
    .value("Leica", FileFormat::Leica);

  py::enum_<Compression>(m, "Compression", py::arithmetic())
    .value("AUTO", Compression::AUTO)
    .value("NONE", Compression::NONE)
    .value("LZW", Compression::LZW)
    .value("JPEG", Compression::JPEG)
    .value("T85", Compression::T85)
    .value("T43", Compression::T43)
    .value("PACKBITS", Compression::PACKBITS)
    .value("DEFLATE", Compression::DEFLATE)
    .value("ADOBE_DEFLATE", Compression::ADOBE_DEFLATE)
    .value("DCS", Compression::DCS)
    .value("JP2000", Compression::JP2000)
    .value("LZMA", Compression::LZMA)
    .value("ZSTD", Compression::ZSTD)
    .value("WEBP", Compression::WEBP)
    .value("JPEGXR", Compression::JPEGXR);

  py::class_<col4>(m, "col4")
    .def(py::init<>())
    .def(py::init<uint8_t, uint8_t, uint8_t, uint8_t>(),
         "r"_a, "g"_a, "b"_a, "a"_a = 255_u8)
    .def_readwrite("r", &col4::r)
    .def_readwrite("g", &col4::g)
    .def_readwrite("b", &col4::b)
    .def_readwrite("a", &col4::a)
    .def("__init__", [](col4& self, py::tuple t) {
      if (py::len(t) != 4)
        throw ZException("col4 needs tuple with 4 values");
      new(&self) col4(t[0].cast<uint8_t>(), t[1].cast<uint8_t>(), t[2].cast<uint8_t>(), t[3].cast<uint8_t>());
    })
    .def("__repr__", [](const col4& v) {
      return fmt::format("<_imgpy.col4 rgba:{}>", v.toString());
    });
  py::implicitly_convertible<py::tuple, col4>();

  static py::exception<ZException> base_ex(m, "ZException");
  static py::exception<ZIOException> io_ex(m, "ZIOException", base_ex.ptr());
  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p) std::rethrow_exception(p);
    }
    catch (const ZIOException& e) {
      io_ex(e.what());
    }
    catch (const ZException& e) {
      base_ex(e.what());
    }
  });

  py::class_<ZImgWriteParameters>(m, "ZImgWriteParameters")
    .def(py::init<>())
    .def_readwrite("compression", &ZImgWriteParameters::compression)
    .def_readwrite("zlibCompressionLevel", &ZImgWriteParameters::zlibCompressionLevel)
    .def_readwrite("jpegQuality", &ZImgWriteParameters::jpegQuality)
    .def_readwrite("jpegProgressive", &ZImgWriteParameters::jpegProgressive)
    .def_readwrite("jpegAccurateDCT", &ZImgWriteParameters::jpegAccurateDCT)
    .def_readwrite("jpegChrominanceSubsampling", &ZImgWriteParameters::jpegChrominanceSubsampling)
    .def_readwrite("jpegXRQuality", &ZImgWriteParameters::jpegXRQuality);

  py::class_<ZImgInfo>(m, "ZImgInfo")
    .def(py::init<>())
    .def(py::init<size_t, size_t, size_t, size_t, size_t, size_t, VoxelFormat>(),
         "width"_a, "height"_a, "depth"_a = 1, "numChannels"_a = 1, "numTimes"_a = 1,
         "bytePerVox"_a = 1, "voxelFormat"_a = VoxelFormat::Unsigned)
    .def_readwrite("width", &ZImgInfo::width)
    .def_readwrite("height", &ZImgInfo::height)
    .def_readwrite("depth", &ZImgInfo::depth)
    .def_readwrite("numChannels", &ZImgInfo::numChannels)
    .def_readwrite("numTimes", &ZImgInfo::numTimes)
    .def_readwrite("bytesPerVoxel", &ZImgInfo::bytesPerVoxel)
    .def_readwrite("voxelFormat", &ZImgInfo::voxelFormat)
    .def_readwrite("validBitCount", &ZImgInfo::validBitCount)
    .def_readwrite("voxelSizeUnit", &ZImgInfo::voxelSizeUnit)
    .def_readwrite("voxelSizeX", &ZImgInfo::voxelSizeX)
    .def_readwrite("voxelSizeY", &ZImgInfo::voxelSizeY)
    .def_readwrite("voxelSizeZ", &ZImgInfo::voxelSizeZ)
    .def_readwrite("timeStamps", &ZImgInfo::timeStamps)
    .def_readwrite("channelNames", &ZImgInfo::channelNames)
    .def_readwrite("channelColors", &ZImgInfo::channelColors)
    .def_readwrite("position", &ZImgInfo::position)
    .def_readwrite("lastChannelIsAlphaChannel", &ZImgInfo::lastChannelIsAlphaChannel)
    .def("voxelSizeXInUm", &ZImgInfo::voxelSizeXInUm)
    .def("voxelSizeYInUm", &ZImgInfo::voxelSizeYInUm)
    .def("voxelSizeZInUm", &ZImgInfo::voxelSizeZInUm)
    .def("dataTypeString", &ZImgInfo::typeAsQString)
    .def("__repr__", [](const ZImgInfo& v) {
      return fmt::format("<_imgpy.ZImgInfo {}>", v.toString());
    });

  py::class_<ZVoxelCoordinate>(m, "ZVoxelCoordinate")
    .def(py::init<>())
    .def(py::init<ZVoxelCoordinate::value_type, ZVoxelCoordinate::value_type, ZVoxelCoordinate::value_type,
           ZVoxelCoordinate::value_type, ZVoxelCoordinate::value_type>(),
         "x"_a, "y"_a, "z"_a = 0, "c"_a = 0, "t"_a = 0)
    .def_readwrite("x", &ZVoxelCoordinate::x)
    .def_readwrite("y", &ZVoxelCoordinate::y)
    .def_readwrite("z", &ZVoxelCoordinate::z)
    .def_readwrite("c", &ZVoxelCoordinate::c)
    .def_readwrite("t", &ZVoxelCoordinate::t)
    .def("__init__", [](ZVoxelCoordinate& self, py::tuple t) {
      if (py::len(t) != 5)
        throw ZException("ZVoxelCoordinate needs tuple with 5 values");
      new(&self) ZVoxelCoordinate(t[0].cast<ZVoxelCoordinate::value_type>(), t[1].cast<ZVoxelCoordinate::value_type>(),
                                  t[2].cast<ZVoxelCoordinate::value_type>(), t[3].cast<ZVoxelCoordinate::value_type>(),
                                  t[4].cast<ZVoxelCoordinate::value_type>());
    })
    .def("__repr__", [](const ZVoxelCoordinate& v) {
      return fmt::format("<_imgpy.ZVoxelCoordinate xyzct:{}>", v.toString());
    });
  py::implicitly_convertible<py::tuple, ZVoxelCoordinate>();

  py::class_<ZImgRegion>(m, "ZImgRegion")
    .def(py::init<>())
    .def(py::init<ZVoxelCoordinate, ZVoxelCoordinate>(), "start"_a, "end"_a)
    .def_readwrite("start", &ZImgRegion::start)
    .def_readwrite("end", &ZImgRegion::end)
    .def("__repr__", [](const ZImgRegion& v) {
      return fmt::format("<_imgpy.ZImgRegion {}>", v.toString());
    });

  py::class_<ZImgSource>(m, "ZImgSource")
    .def(py::init<>())
    .def(py::init<const QString&, const ZImgRegion&, size_t, FileFormat>(),
         "filename"_a, "region"_a = ZImgRegion(), "scene"_a = 0, "format"_a = FileFormat::Unknown)
    .def(py::init<const QStringList&, Dimension, bool, const ZImgRegion&, size_t, FileFormat, bool, bool>(),
         "filenames"_a, "catDim"_a, "catScenes"_a = true, "region"_a = ZImgRegion(), "scene"_a = 0,
         "format"_a = FileFormat::Unknown,
         "expandXY"_a = true, "expandWithMaxValue"_a = false)
    .def_readwrite("filenames", &ZImgSource::filenames)
    .def_readwrite("catDim", &ZImgSource::catDim)
    .def_readwrite("region", &ZImgSource::region)
    .def_readwrite("scene", &ZImgSource::scene)
    .def_readwrite("format", &ZImgSource::format)
    .def_readwrite("expandXY", &ZImgSource::expandXY)
    .def_readwrite("expandWithMaxValue", &ZImgSource::expandWithMaxValue)
    .def_readwrite("totalFileSize", &ZImgSource::totalFileSize)
    .def_readwrite("catScenes", &ZImgSource::catScenes)
    .def("__repr__", [](const ZImgSource& v) {
      return fmt::format("<_imgpy.ZImgSource {}>", v.toString());
    });

  py::class_<ZImg>(m, "ZImg")
    .def(py::init<>())
    .def(py::init<const ZImgInfo&>())
    .def(py::init<const QString&, ZImgRegion, size_t, size_t, size_t, size_t, FileFormat>(),
         "filename"_a, "region"_a = ZImgRegion(), "scene"_a = 0, "xRatio"_a = 1, "yRatio"_a = 1, "zRatio"_a = 1,
         "format"_a = FileFormat::Unknown)
    .def(py::init<>([](const QStringList& fileList, Dimension catDim, bool catScenes, const ZImgRegion& region, size_t scene,
                       size_t xRatio, size_t yRatio, size_t zRatio,
                       FileFormat format, bool expandXY, bool expandWithMaxValue) {
           return new ZImg(fileList, catDim, catScenes, region, scene, xRatio, yRatio, zRatio,
                           format, expandXY, expandWithMaxValue);
         }),
         "filenames"_a, "catDim"_a, "catScenes"_a, "region"_a = ZImgRegion(), "scene"_a = 0,
         "xRatio"_a = 1, "yRatio"_a = 1, "zRatio"_a = 1,
         "format"_a = FileFormat::Unknown,
         "expandXY"_a = true, "expandWithMaxValue"_a = false)
    .def(py::init<const ZImgSource&, size_t, size_t, size_t>(),
         "imgSource"_a, "xRatio"_a = 1, "yRatio"_a = 1, "zRatio"_a = 1)
    .def(py::init<>([](const py::array& arr, const ZImgInfo& info_in) {
      auto img = new ZImg();
      auto info = getImgInfoFromNdarray(arr, info_in);
      img->wrapData(const_cast<void*>(arr.data()), info);
      return img;
    }), "ndarray"_a, "imgInfo"_a = ZImgInfo())
    .def(py::init<>([](const std::vector<py::array>& arrs, const ZImgInfo& info_in) {
      auto img = new ZImg();
      std::vector<void*> data;
      if (!arrs.empty()) {
        auto info = getImgInfoFromNdarray(arrs[0], info_in);
        data.push_back(const_cast<void*>(arrs[0].data()));
        for (size_t t = 1; t < arrs.size(); ++t) {
          auto tmpinfo = getImgInfoFromNdarray(arrs[t], info_in);
          if (!tmpinfo.isSameType(info) || !tmpinfo.isSameSize(info)) {
            throw ZException("ndarrays in the list are not compatible");
          }
          data.push_back(const_cast<void*>(arrs[t].data()));
        }

        info.numTimes = arrs.size();
        img->wrapData(data, info);
      }
      return img;
    }), "listOfndarray"_a, "imgInfo"_a = ZImgInfo())
    .def_static("readImgInfos", [](const QString& filename, FileFormat format) {
      return ZImg::readImgInfos(filename, nullptr, format);
    }, "filename"_a, "format"_a = FileFormat::Unknown)
    .def_static("readImgInfos", [](const QStringList& fileList, Dimension catDim, bool catScenes, FileFormat format, bool expandXY) {
      return ZImg::readImgInfos(fileList, catDim, catScenes, nullptr, format, expandXY);
    }, "filenames"_a, "catDim"_a, "catScenes"_a, "format"_a = FileFormat::Unknown, "expandXY"_a = true)
    .def_static("readImgInfo", [](const ZImgSource& imgSource) {
      return ZImg::readImgInfo(imgSource);
    })
    .def_static("readSubBlockLists",
                [](const QString& filename, FileFormat format) {
                  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
                  ZImg::readImgInfos(filename, &subBlocks, format);
                  std::vector<Eigen::Matrix<int64_t, Eigen::Dynamic, Eigen::Dynamic>> res(subBlocks.size());
                  for (size_t s = 0; s < res.size(); ++s) {
                    auto& mat = res[s];
                    const auto& blocks = subBlocks[s];
                    mat.resize(blocks.size(), 10);
                    for (Eigen::Index r = 0; r < mat.rows(); ++r) {
                      const auto& block = blocks[r];
                      mat(r, 0) = block->t;
                      mat(r, 1) = block->x;
                      mat(r, 2) = block->y;
                      mat(r, 3) = block->z;
                      mat(r, 4) = block->width;
                      mat(r, 5) = block->height;
                      mat(r, 6) = block->depth;
                      mat(r, 7) = block->xRatio;
                      mat(r, 8) = block->yRatio;
                      mat(r, 9) = block->zRatio;
                    }
                  }
                  return res;
                }, "filename"_a, "format"_a = FileFormat::Unknown)
    .def_static("readSubBlockLists",
                [](const QStringList& fileList, Dimension catDim, bool catScenes, FileFormat format, bool expandXY) {
                  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
                  ZImg::readImgInfos(fileList, catDim, catScenes, &subBlocks, format, expandXY);
                  std::vector<Eigen::Matrix<int64_t, Eigen::Dynamic, Eigen::Dynamic>> res(subBlocks.size());
                  for (size_t s = 0; s < res.size(); ++s) {
                    auto& mat = res[s];
                    const auto& blocks = subBlocks[s];
                    mat.resize(blocks.size(), 10);
                    for (Eigen::Index r = 0; r < mat.rows(); ++r) {
                      const auto& block = blocks[r];
                      mat(r, 0) = block->t;
                      mat(r, 1) = block->x;
                      mat(r, 2) = block->y;
                      mat(r, 3) = block->z;
                      mat(r, 4) = block->width;
                      mat(r, 5) = block->height;
                      mat(r, 6) = block->depth;
                      mat(r, 7) = block->xRatio;
                      mat(r, 8) = block->yRatio;
                      mat(r, 9) = block->zRatio;
                    }
                  }
                  return res;
                }, "filenames"_a, "catDim"_a, "catScenes"_a, "format"_a = FileFormat::Unknown, "expandXY"_a = true)
    .def_static("readSubBlock", [](const QString& filename, size_t scene, size_t blockIndex, FileFormat format) {
      return ZImg::readSubBlock(filename, scene, blockIndex, format);
    }, "filename"_a, "scene"_a, "blockIndex"_a, "format"_a = FileFormat::Unknown)
    .def_static("readSubBlock", [](const QStringList& fileList, Dimension catDim, bool catScenes, size_t scene, size_t blockIndex,
                                   FileFormat format, bool expandXY) {
      return ZImg::readSubBlock(fileList, catDim, catScenes, scene, blockIndex, format, expandXY);
    }, "filenames"_a, "catDim"_a, "catScenes"_a, "scene"_a, "blockIndex"_a, "format"_a = FileFormat::Unknown, "expandXY"_a = true)
    .def_static("getInternalSubRegions", [](const QString& filename, FileFormat format) {
      return ZImg::getInternalSubRegions(filename, format);
    }, "filename"_a, "format"_a = FileFormat::Unknown)
    .def("isEmpty", &ZImg::isEmpty)
    .def("save", &ZImg::save,
         "filename"_a, "format"_a = FileFormat::Unknown, "paras"_a = ZImgWriteParameters())
    .def_static("writeImg", py::overload_cast<const QString&, const ZImgSliceProvider&, FileFormat, const ZImgWriteParameters&>(&ZImg::writeImg),
                "filename"_a, "sliceProvider"_a, "format"_a = FileFormat::Unknown, "paras"_a = ZImgWriteParameters())
    .def_static("writeImg", py::overload_cast<const QString&, const ZImgBlockProvider&, FileFormat, const ZImgWriteParameters&>(&ZImg::writeImg),
                "filename"_a, "blockProvider"_a, "format"_a = FileFormat::Unknown, "paras"_a = ZImgWriteParameters())
    .def_property("info",
                  [](const ZImg& v) {
                    return v.info();
                  },
                  [](ZImg& v, const ZImgInfo& info) {
                    v.infoRef() = info;
                  })
    .def_property_readonly("data",
                           [](ZImg& v) {
                             std::vector<py::array> arrs;
                             if (!v.isEmpty()) {
                               auto formatdesc = getFormatDesc(v);
                               for (size_t t = 0; t < v.numTimes(); ++t) {
                                 auto capsule = py::capsule(v.timeData(t), [](void*) {});
                                 // use do nothing capsule so array does not copy or delete data, data still owned by v
                                 arrs.push_back(py::array(getDType(v),
                                                          {v.numChannels(), v.depth(), v.height(), v.width()},
                                                          v.timeData(t),
                                                          capsule));
                               }
                             }
                             return arrs;
                           })
    .def("resize", &ZImg::resize,
         "desWidth"_a, "desHeight"_a, "desDepth"_a, "interpolant"_a = Interpolant::Cubic, "antialiasing"_a = true,
         "antialiasingForNearest"_a = false, "useMultithreading"_a = true)
    .def("zoom", &ZImg::zoom,
         "scaleX"_a, "scaleY"_a, "scaleZ"_a = 1.0, "interpolant"_a = Interpolant::Cubic, "antialiasing"_a = true,
         "antialiasingForNearest"_a = false)
    .def("blockDownsample", &ZImg::blockDownsample,
         "blockWidth"_a, "blockHeight"_a, "blockDepth"_a, "mergeMode"_a)
    .def("secureDivideBy", &ZImg::secureDivideBy,
         "rhs"_a)
    .def(py::self + py::self)
    .def(py::self += py::self)
    .def(py::self - py::self)
#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-assign-overloaded"
#endif
    .def(py::self -= py::self)
#ifdef __clang__
#pragma GCC diagnostic pop
#endif
    .def(py::self * py::self)
    .def(py::self *= py::self)
    .def(py::self / py::self)
#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-assign-overloaded"
#endif
    .def(py::self /= py::self)
#ifdef __clang__
#pragma GCC diagnostic pop
#endif
    .def(py::self + double())
    .def(py::self += double())
    .def(py::self - double())
    .def(py::self -= double())
    .def(py::self * double())
    .def(py::self *= double())
    .def(py::self / double())
    .def(py::self /= double())
    .def(py::self + int32_t())
    .def(py::self += int32_t())
    .def(py::self - int32_t())
    .def(py::self -= int32_t())
    .def(py::self * int32_t())
    .def(py::self *= int32_t())
    .def(py::self / int32_t())
    .def(py::self /= int32_t())
    .def(py::self + uint32_t())
    .def(py::self += uint32_t())
    .def(py::self - uint32_t())
    .def(py::self -= uint32_t())
    .def(py::self * uint32_t())
    .def(py::self *= uint32_t())
    .def(py::self / uint32_t())
    .def(py::self /= uint32_t())
    .def(py::self + int64_t())
    .def(py::self += int64_t())
    .def(py::self - int64_t())
    .def(py::self -= int64_t())
    .def(py::self * int64_t())
    .def(py::self *= int64_t())
    .def(py::self / int64_t())
    .def(py::self /= int64_t())
    .def(py::self + uint64_t())
    .def(py::self += uint64_t())
    .def(py::self - uint64_t())
    .def(py::self -= uint64_t())
    .def(py::self * uint64_t())
    .def(py::self *= uint64_t())
    .def(py::self / uint64_t())
    .def(py::self /= uint64_t())
    .def("__repr__", [](const ZImg& v) {
      return fmt::format("<_imgpy.ZImg {}>", v.info().toString());
    });

  py::class_<ZPunctum>(m, "ZPunctum")
    .def(py::init<>())
    .def(py::init<double, double, double, double>(), "x"_a, "y"_a, "z"_a, "r"_a)
    .def(py::init<const Eigen::MatrixXi&, const Eigen::VectorXd&>(), "voxelLocations"_a, "voxelIntensities"_a)
    .def_property("name", &ZPunctum::name, &ZPunctum::setName)
    .def_property("comment", &ZPunctum::comment, &ZPunctum::setComment)
    .def_property("x", &ZPunctum::x, &ZPunctum::setX)
    .def_property("y", &ZPunctum::y, &ZPunctum::setY)
    .def_property("z", &ZPunctum::z, &ZPunctum::setZ)
    .def_property("maxIntensity", &ZPunctum::maxIntensity, &ZPunctum::setMaxIntensity)
    .def_property("meanIntensity", &ZPunctum::meanIntensity, &ZPunctum::setMeanIntensity)
    .def_property("sDevOfIntensity", &ZPunctum::sDevOfIntensity, &ZPunctum::setSDevOfIntensity)
    .def_property("volSize", &ZPunctum::volSize, &ZPunctum::setVolSize)
    .def_property("mass", &ZPunctum::mass, &ZPunctum::setMass)
    .def_property("radius", &ZPunctum::radius, &ZPunctum::setRadius)
    .def_property("property1", &ZPunctum::property1, &ZPunctum::setProperty1)
    .def_property("property2", &ZPunctum::property2, &ZPunctum::setProperty2)
    .def_property("property3", &ZPunctum::property3, &ZPunctum::setProperty3)
    .def_property("color", &ZPunctum::color, &ZPunctum::setColor)
    .def_property("score", &ZPunctum::score, &ZPunctum::setScore)
    .def_property("voxelLocations", &ZPunctum::voxelLocations, &ZPunctum::setVoxelLocations)
    .def_property("voxelIntensities", &ZPunctum::voxelIntensities, &ZPunctum::setVoxelIntensities)
    .def("updateFromVoxelsList", &ZPunctum::updateFromVoxelsList, "conf"_a = 0.95)
    .def("containsSignal", &ZPunctum::containsSignal)
    .def("mergeWith", &ZPunctum::mergeWith, "otherPunctum"_a, "conf"_a = 0.95)
    .def("split", &ZPunctum::split, "num"_a, "conf"_a = 0.95)
    .def_static("merge", [](const std::list<ZPunctum>& punctumList, double conf) {
      return ZPunctum::merge(punctumList.begin(), punctumList.end(), conf);
    }, "punctumList"_a, "conf"_a = 0.95)
    .def("__repr__", [](const ZPunctum& v) {
      return fmt::format("<_imgpy.ZPunctum {}>", v.toString());
    });

  py::class_<ZPuncta>(m, "ZPuncta")
    .def(py::init<>())
    .def(py::init<const std::list<ZPunctum>&>())
    .def(py::init<const QString&>(), "filename"_a)
    .def_readwrite("data", &ZPuncta::data)
    .def("save", &ZPuncta::save,
         "filename"_a, "format"_a = QString())
    .def("__repr__", [](const ZPuncta& v) {
      return fmt::format("<_imgpy.ZPuncta {}>", v.toString());
    });

  py::class_<ZStitchImage>(m, "ZStitchImage")
    .def(py::init<>())
    .def("setInputFilenames", &ZStitchImage::setInputFilenames,
         "filenames"_a, "scene"_a = 0)
    .def("setResultFilename", &ZStitchImage::setResultFilename,
         "filename"_a)
    .def("setUseChannels", &ZStitchImage::setUseChannels,
         "channels"_a = std::vector<size_t>())
    .def("setRemoveBackgroundForChannels", &ZStitchImage::setRemoveBackgroundForChannels,
         "channels"_a = std::vector<size_t>())
    .def("setDownsampleBeforeStitching", &ZStitchImage::setDownsampleBeforeStitching,
         "blockWidth"_a, "blockHeight"_a, "blockDepth"_a, "blockMergeMode"_a = ImgMergeMode::Mean)
    .def("setStartResolution", &ZStitchImage::setStartResolution,
         "intvX"_a, "intvY"_a, "intvZ"_a)
    .def("setConcatenateOnly", &ZStitchImage::setConcatenateOnly)
    .def("setMergeMode", &ZStitchImage::setMergeMode,
         "mode"_a)
    .def("setMaxOverlapRate", &ZStitchImage::setMaxOverlapRate,
         "maxOverlapRate"_a)
    .def("setTileGrid", &ZStitchImage::setTileGrid,
         "tileGrid"_a)
    .def("setTileGridFromMatrixFile", &ZStitchImage::setTileGridFromMatrixFile,
         "filename"_a)
    .def("setTileGridFromTileSelectionImage", &ZStitchImage::setTileGridFromTileSelectionImage,
         "filename"_a)
    .def("setConnInfoFromConnTextFile", &ZStitchImage::setConnInfoFromConnTextFile,
         "filename"_a)
    .def("setTileGridFromLayout", &ZStitchImage::setTileGridFromLayout,
         "numRows"_a, "numCols"_a)
    .def("setRestitch", &ZStitchImage::setRestitch)
    .def("setBlindStitching", &ZStitchImage::setBlindStitching)
    .def("set2ndInput", &ZStitchImage::set2ndInput,
         "fns"_a, "scene"_a, "chsToUse"_a, "chsToRemoveBackground"_a, "commonChannelOfInput"_a,
         "commonChannelOf2ndInput"_a)
    .def("setUseMultithreading", &ZStitchImage::setUseMultithreading,
         "v"_a)
    .def("setLogFile", &ZStitchImage::setLogFile, "logfilename"_a)
    .def("loadTask", &ZStitchImage::loadTask, "filename"_a)
    .def("saveTask", &ZStitchImage::saveTask, "filename"_a)
    .def("run", &ZStitchImage::runInPython)
    .def("__repr__", [](const ZStitchImage& v) {
      return fmt::format("<_imgpy.ZStitchImage {}>", v.toString());
    });

  py::class_<ZPunctaDetection>(m, "ZPunctaDetection")
    .def(py::init<>())
    .def(py::init<const QString&, size_t, size_t, size_t>(),
         "filename"_a, "punctaChannel"_a = 0, "t"_a = 0, "scene"_a = 0)
    .def(py::init<const QString&, const ZImgInfo&, size_t, size_t, size_t>(),
         "filename"_a, "imgInfo"_a, "punctaChannel"_a = 0, "t"_a = 0, "scene"_a = 0)
    .def("setInputFile", &ZPunctaDetection::setInputFile,
         "filename"_a, "punctaChannel"_a = 0, "t"_a = 0, "scene"_a = 0,
         "voxelSizeInUmX"_a = -1.0, "voxelSizeInUmY"_a = -1.0, "voxelSizeInUmZ"_a = -1.0)
    .def("setResultPunctaFilename", &ZPunctaDetection::setResultPunctaFilename,
         "filename"_a)
    .def("setResultSomaPunctaFilename", &ZPunctaDetection::setResultSomaPunctaFilename,
         "filename"_a)
    .def("setPunctaThreshold", &ZPunctaDetection::setPunctaThreshold,
         "thre"_a)
    .def("setSomaPunctaThreshold", &ZPunctaDetection::setSomaPunctaThreshold,
         "thre"_a)
    .def("setSplitThreshold", &ZPunctaDetection::setSplitThreshold, "thre"_a)
    .def("setConfidenceRegionForRadiusEstimate", &ZPunctaDetection::setConfidenceRegionForRadiusEstimate,
         "confRadius"_a)
    .def("setConfidenceRegionForOverlapArea", &ZPunctaDetection::setConfidenceRegionForOverlapArea,
         "confOverlapArea"_a)
    .def("setOverlapRateThreshold", &ZPunctaDetection::setOverlapRateThreshold,
         "thre"_a)
    .def("setSeedSizeThreshold", &ZPunctaDetection::setSeedSizeThreshold,
         "thre"_a)
    .def("setUseMultithreading", &ZPunctaDetection::setUseMultithreading,
         "v"_a)
    .def("setDendriteChannel", &ZPunctaDetection::setDendriteChannel,
         "dendriteChannel"_a)
    .def("setMaxDendriteTubeRadiusInUm", &ZPunctaDetection::setMaxDendriteTubeRadiusInUm,
         "maxDendriteTubeRadius"_a)
    .def("setDendriteThreshold", &ZPunctaDetection::setDendriteThreshold,
         "thre"_a)
    .def("setSwcFiles", &ZPunctaDetection::setSwcFiles,
         "swcFiles"_a)
    .def("setMaxDistToBranchInUm", &ZPunctaDetection::setMaxDistToBranchInUm,
         "dist"_a)
    .def("setAmbiguousFactor", &ZPunctaDetection::setAmbiguousFactor,
         "factor"_a)
    .def("setLogFile", &ZPunctaDetection::setLogFile, "logfilename"_a)
    .def("loadTask", &ZPunctaDetection::loadTask, "filename"_a)
    .def("saveTask", &ZPunctaDetection::saveTask, "filename"_a)
    .def("run", &ZPunctaDetection::runInPython)
    .def("__repr__", [](const ZPunctaDetection& v) {
      return fmt::format("<_imgpy.ZPunctaDetection {}>", v.toString());
    });

  py::class_<ZSectionsRegistration>(m, "ZSectionsRegistration")
    .def(py::init<>())
    .def("setInputOutput", &ZSectionsRegistration::setInputOutput,
         "inputFiles"_a, "resultFile"_a, "fixedSliceIndex"_a)
    .def("setReferenceChannel", &ZSectionsRegistration::setReferenceChannel,
         "refChannel"_a)
    .def("setRemoveBackground", &ZSectionsRegistration::setRemoveBackground,
         "v"_a)
    .def("setRemoveHighForeground", &ZSectionsRegistration::setRemoveHighForeground,
         "v"_a)
    .def("setAllowFlip", &ZSectionsRegistration::setAllowFlip, "v"_a)
    .def("setBrightBackground", &ZSectionsRegistration::setBrightBackground,
         "v"_a)
    .def("setMetric", &ZSectionsRegistration::setMetric,
         "metric"_a)
    .def("setTransform", &ZSectionsRegistration::setTransform,
         "transform"_a)
    .def("setOptimizer", &ZSectionsRegistration::setOptimizer,
         "optimizer"_a)
    .def("setUseMultithreading", &ZSectionsRegistration::setUseMultithreading,
         "v"_a)
    .def("setNumScales", &ZSectionsRegistration::setNumScales,
         "numScales"_a)
    .def("setNumNeighbors", &ZSectionsRegistration::setNumNeighbors,
         "numNeighbors"_a)
    .def("setLogFile", &ZSectionsRegistration::setLogFile, "logfilename"_a)
    .def("loadTask", &ZSectionsRegistration::loadTask, "filename"_a)
    .def("saveTask", &ZSectionsRegistration::saveTask, "filename"_a)
    .def("run", &ZSectionsRegistration::runInPython)
    .def("__repr__", [](const ZSectionsRegistration& v) {
      return fmt::format("<_imgpy.ZSectionsRegistration {}>", v.toString());
    });

  py::class_<ZChromaticShiftCorrection>(m, "ZChromaticShiftCorrection")
    .def(py::init<>())
    .def("setInputOutput", &ZChromaticShiftCorrection::setInputOutput,
         "inputFile"_a, "resultFile"_a)
    .def("setReferenceChannel", &ZChromaticShiftCorrection::setReferenceChannel,
         "refChannel"_a)
    .def("setTargetChannel", &ZChromaticShiftCorrection::setTargetChannel,
         "targetChannel"_a)
    .def("setRemoveBackground", &ZChromaticShiftCorrection::setRemoveBackground,
         "v"_a)
    .def("setRemoveHighForeground", &ZChromaticShiftCorrection::setRemoveHighForeground,
         "v"_a)
    .def("setBrightBackground", &ZChromaticShiftCorrection::setBrightBackground,
         "v"_a)
    .def("setMethod", &ZChromaticShiftCorrection::setMethod,
         "method"_a)
    .def("setMetric", &ZChromaticShiftCorrection::setMetric,
         "metric"_a)
    .def("setTransform", &ZChromaticShiftCorrection::setTransform,
         "transform"_a)
    .def("setOptimizer", &ZChromaticShiftCorrection::setOptimizer,
         "optimizer"_a)
    .def("setUseMultithreading", &ZChromaticShiftCorrection::setUseMultithreading,
         "v"_a)
    .def("setNumScales", &ZChromaticShiftCorrection::setNumScales,
         "numScales"_a)
    .def("setLogFile", &ZChromaticShiftCorrection::setLogFile, "logfilename"_a)
    .def("loadTask", &ZChromaticShiftCorrection::loadTask, "filename"_a)
    .def("saveTask", &ZChromaticShiftCorrection::saveTask, "filename"_a)
    .def("run", &ZChromaticShiftCorrection::runInPython)
    .def("__repr__", [](const ZChromaticShiftCorrection& v) {
      return fmt::format("<_imgpy.ZChromaticShiftCorrection {}>", v.toString());
    });

  py::class_<ZImgNCCMatch>(m, "ZImgNCCMatch")
    .def(py::init<const ZImg&, const ZImg&, size_t, size_t>(),
         "fixedImg"_a, "movingImg"_a, "fixedT"_a = 0, "movingT"_a = 0)
    .def("useFixedImgChannels", &ZImgNCCMatch::useFixedImgChannels,
         "chs"_a)
    .def("useMovingImgChannels", &ZImgNCCMatch::useMovingImgChannels,
         "chs"_a)
    .def("useAllFixedImgChannels", &ZImgNCCMatch::useAllFixedImgChannels)
    .def("useAllMovingImgChannels", &ZImgNCCMatch::useAllMovingImgChannels)
    .def("removeBackgroundForFixedImgChannels", &ZImgNCCMatch::removeBackgroundForFixedImgChannels,
         "chs"_a)
    .def("removeBackgroundForMovingImgChannels", &ZImgNCCMatch::removeBackgroundForMovingImgChannels,
         "chs"_a)
    .def("disableRemoveBackgroundForAllFixedImgChannels", &ZImgNCCMatch::disableRemoveBackgroundForAllFixedImgChannels)
    .def("disableRemoveBackgroundForAllMovingImgChannels",
         &ZImgNCCMatch::disableRemoveBackgroundForAllMovingImgChannels)
    .def("computeNCCOfOffset", &ZImgNCCMatch::computeNCCOfOffset,
         "offset"_a)
    .def("computeNCC", &ZImgNCCMatch::computeNCC)
    .def("computeMovingImgOffset", &ZImgNCCMatch::computeMovingImgOffset_Python)
    .def("computeMovingImgOffsetMR", &ZImgNCCMatch::computeMovingImgOffsetMR_Python,
         "intvX"_a, "intvY"_a, "intvZ"_a)
    .def("refineMovingImgOffset", &ZImgNCCMatch::refineMovingImgOffset_Python,
         "offset"_a, "radiusX"_a, "radiusY"_a, "radiusZ"_a)
    .def("refineMovingImgOffsetMR", &ZImgNCCMatch::refineMovingImgOffsetMR_Python,
         "offset"_a, "radiusX"_a, "radiusY"_a, "radiusZ"_a, "intvX"_a, "intvY"_a, "intvZ"_a)
    .def("__repr__", [](const ZImgNCCMatch&) {
      return fmt::format("<_imgpy.ZImgNCCMatch>");
    });

  py::class_<ZROIUtils>(m, "ZROIUtils")
    .def_static("splineToMask", &ZROIUtils::splineToMask,
                "spline"_a.noconvert())
    .def_static("rectToMask", &ZROIUtils::rectToMask,
                "rect"_a.noconvert())
    .def_static("ellipseToMask", &ZROIUtils::ellipseToMask,
                "ellipse"_a.noconvert())
    .def_static("polygonToMask", &ZROIUtils::polygonToMask,
                "polygon"_a.noconvert())
    .def_static("shapeToMask", &ZROIUtils::shapeToMask,
                "shapes"_a)
    .def("__repr__", [](const ZROIUtils&) {
      return fmt::format("<_imgpy.ZROIUtils>");
    });

  py::class_<ZImgSubBlock, PyZImgSubBlock<>>(m, "ZImgSubBlock")
    .def(py::init<size_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, size_t, size_t, size_t>(),
         "t"_a, "x"_a, "y"_a, "z"_a, "width"_a, "height"_a, "depth"_a, "xRatio"_a, "yRatio"_a, "zRatio"_a)
    .def("read", &ZImgSubBlock::read)
    .def("readInfo", &ZImgSubBlock::readInfo)
    .def("__repr__", [](const ZImgSubBlock&) {
      return fmt::format("<_imgpy.ZImgSubBlock>");
    });
  py::class_<ZImgTileSubBlock, ZImgSubBlock, PyZImgTileSubBlock<>>(m, "ZImgTileSubBlock")
    .def(py::init<const ZImgSource&, size_t, size_t, size_t, ImgMergeMode>(),
         "source"_a, "xRatio"_a = 1, "yRatio"_a = 1, "zRatio"_a = 1,
         "downsampleCombineMode"_a = ImgMergeMode::Interpolation)
    .def("__repr__", [](const ZImgTileSubBlock&) {
      return fmt::format("<_imgpy.ZImgTileSubBlock>");
    });

  py::class_<ZImgSliceProvider, PyZImgSliceProvider>(m, "ZImgSliceProvider")
    .def(py::init<>())
    .def("imgInfo", &ZImgSliceProvider::imgInfo)
    .def("slice", &ZImgSliceProvider::slice, "z"_a, "t"_a)
    .def("allSlices", &ZImgSliceProvider::allSlices, "t"_a)
    .def("wholeImg", &ZImgSliceProvider::wholeImg)
    .def("__repr__", [](const ZImgSliceProvider&) {
      return fmt::format("<_imgpy.ZImgSliceProvider>");
    });

  py::class_<ZImgBlockProvider, PyZImgBlockProvider>(m, "ZImgBlockProvider")
    .def(py::init<>())
    .def("imgInfo", &ZImgBlockProvider::imgInfo)
    .def("numBlocks", &ZImgBlockProvider::numBlocks)
    .def("block", &ZImgBlockProvider::block, "blockIdx"_a)
    .def("blockCoord", &ZImgBlockProvider::blockCoord, "blockIdx"_a)
    .def("wholeImg", &ZImgBlockProvider::wholeImg)
    .def("__repr__", [](const ZImgBlockProvider&) {
      return fmt::format("<_imgpy.ZImgBlockProvider>");
    });

  py::class_<ZImgMerge>(m, "ZImgMerge")
    .def(py::init<>())
    .def("addImg", &ZImgMerge::addImg,
         "img"_a, "loc"_a, "imgName"_a = QString(""))
    .def("addImgPair", &ZImgMerge::addImgPair,
         "img1"_a, "img2"_a, "img2Offset"_a, "connectionCost"_a = 0, "img1Name"_a = QString(""), "img2Name"_a = QString(""))
    .def("resolveLocations", &ZImgMerge::resolveLocations)
    .def("setMergeMode", &ZImgMerge::setMergeMode,
         "mode"_a = ImgMergeMode::Max)
    .def("save", &ZImgMerge::save,
         "filename"_a, "format"_a = FileFormat::Unknown, "paras"_a = ZImgWriteParameters())
    .def("__repr__", [](const ZImgMerge&) {
      return fmt::format("<_imgpy.ZImgMerge>");
    });

  py::class_<ZImgAutoThreshold<false>>(m, "ZImgAutoThreshold")
    .def(py::init<>())
    .def("u8TriangleThre", &ZImgAutoThreshold<false>::u8TriangleThre,
         "filename"_a, "minValue"_a, "maxValue"_a, "c"_a = 0, "t"_a = 0, "scene"_a = 0,
         "mask"_a = std::vector<ZVoxelCoordinate>())
    .def("__repr__", [](const ZImgAutoThreshold<false>&) {
      return fmt::format("<_imgpy.ZImgAutoThreshold>");
    });

  py::class_<SwcNode>(m, "SwcNode")
    .def(py::init<int64_t, int64_t, double, double, double, double, int64_t>(),
         "id"_a = -1, "type"_a = -1, "x"_a = 0., "y"_a = 0., "z"_a = 0., "radius"_a = -1., "parentID"_a = -2)
    .def_readwrite("id", &SwcNode::id)
    .def_readwrite("type", &SwcNode::type)
    .def_readwrite("x", &SwcNode::x)
    .def_readwrite("y", &SwcNode::y)
    .def_readwrite("z", &SwcNode::z)
    .def_readwrite("radius", &SwcNode::radius)
    .def_readwrite("parentID", &SwcNode::parentID)
    .def_readwrite("label", &SwcNode::label)
    .def("__repr__", [](const SwcNode& v) {
      return fmt::format("<_imgpy.SwcNode {}>", v.toString());
    });

  py::class_<ZSwc>(m, "ZSwc")
    .def(py::init<>())
    .def(py::init<const QString&>(), "filename"_a)
    .def("load", &ZSwc::load, "filename"_a)
    .def("save", &ZSwc::save, "filename"_a)
    .def("labelSomaAndOthers", &ZSwc::labelSomaAndOthers,
         "radiusThre"_a = 0., "somaType"_a = 1, "otherType"_a = 2)
    .def("resortPyramidal", &ZSwc::resortPyramidal,
         "basalType"_a = 3, "apicalType"_a = 4, "somaType"_a = 1)
    .def("resortID", &ZSwc::resortID)
    .def("__repr__", [](const ZSwc& v) {
      return fmt::format("<_imgpy.ZSwc {}>", v.toString());
    });

  py::enum_<ZMesh::Type>(m, "ZMeshType")
    .value("TRIANGLES", ZMesh::Type::TRIANGLES)
    .value("TRIANGLE_STRIP", ZMesh::Type::TRIANGLE_STRIP)
    .value("TRIANGLE_FAN", ZMesh::Type::TRIANGLE_FAN);

  py::class_<ZMesh>(m, "ZMesh")
    .def(py::init<ZMesh::Type>(), "type"_a = ZMesh::Type::TRIANGLES)
    .def(py::init<const QString&>(), "filename"_a)
    .def("load", py::overload_cast<const QString&>(&ZMesh::load),
      "filename"_a)
    .def("save", py::overload_cast<const QString&, const std::string&>(&ZMesh::save, py::const_),
      "filename"_a, "format"_a = std::string())
    .def("toLabelImg", &ZMesh::toLabelImg,
         "width"_a = 0, "height"_a = 0, "depth"_a = 0, "tfmat"_a = glm::mat4(1.f), "tolerance"_a = 1e-6)
    .def_property("type", &ZMesh::type, &ZMesh::setType)
    .def_static("createPunctaMesh", [](const ZPuncta& puncta, int resolution, const glm::mat4& tfmat) {
        ZMesh res;
        ZMesh::createPunctaMesh(puncta, res, resolution, tfmat);
        return res;
    }, "puncta"_a, "resolution"_a = 32, "tfmat"_a = glm::mat4(1.f))
    .def_static("createSwcMesh", [](const ZSwc& swc, int somaType, const glm::mat4& tfmat) {
        ZMesh rootMesh, somaMesh, neuriteMesh;
        ZMesh::createSwcMesh(swc, somaType, rootMesh, somaMesh, neuriteMesh, tfmat);
        return std::make_tuple(rootMesh, somaMesh, neuriteMesh);
    }, "swc"_a, "somaType"_a = 1, "tfmat"_a = glm::mat4(1.f))
    .def_property("vertices",
                  [](const ZMesh& v) {
                    return vecVecToArray(v.vertices());
                  },
                  [](ZMesh& v, const py::array_t<float, py::array::c_style | py::array::forcecast>& array) {
                    v.setVertices(arrayToVecVec<3>(array));
                  })
    .def_property("normals",
                  [](const ZMesh& v) {
                    return vecVecToArray(v.normals());
                  },
                  [](ZMesh& v, const py::array_t<float, py::array::c_style | py::array::forcecast>& array) {
                    v.setNormals(arrayToVecVec<3>(array));
                  })
    .def_property("colors",
                  [](const ZMesh& v) {
                    return vecVecToArray(v.colors());
                  },
                  [](ZMesh& v, const py::array_t<float, py::array::c_style | py::array::forcecast>& array) {
                    v.setColors(arrayToVecVec<4>(array));
                  })
    .def_property("indices",
                  [](const ZMesh& v) {
                    return vectorToArray(v.indices());
                  },
                  [](ZMesh& v, const py::array_t<uint32_t, py::array::c_style | py::array::forcecast>& array) {
                    v.setIndices(arrayToVector(array));
                  })
    .def_property("textureCoordinates1D",
                  [](const ZMesh& v) {
                    return vectorToArray(v.textureCoordinates1D());
                  },
                  [](ZMesh& v, const py::array_t<float, py::array::c_style | py::array::forcecast>& array) {
                    v.setTextureCoordinates(arrayToVector(array));
                  })
    .def_property("textureCoordinates2D",
                  [](const ZMesh& v) {
                    return vecVecToArray(v.textureCoordinates2D());
                  },
                  [](ZMesh& v, const py::array_t<float, py::array::c_style | py::array::forcecast>& array) {
                    v.setTextureCoordinates(arrayToVecVec<2>(array));
                  })
    .def_property("textureCoordinates3D",
                  [](const ZMesh& v) {
                    return vecVecToArray(v.textureCoordinates3D());
                  },
                  [](ZMesh& v, const py::array_t<float, py::array::c_style | py::array::forcecast>& array) {
                    v.setTextureCoordinates(arrayToVecVec<3>(array));
                  })
    .def("__repr__", [](const ZMesh& v) {
      return fmt::format("<_imgpy.ZMesh {}>", v.toString());
    });

  m.attr("__version__") = GIT_VERSION;

//  auto cleanup_callback = []() {
//    // perform cleanup here -- this function is called with the GIL held
//    shutdownImgLib();
//  };
//
//  m.add_object("_cleanup", py::capsule(cleanup_callback));
}
