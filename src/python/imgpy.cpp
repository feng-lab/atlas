#include "../version/version.h"
#include "typecast.h"
#include "ndarray_utils.h"
#include "zimg.h"
#include "zpuncta.h"
#include "zbioformatsbridgeclient.h"
#include "zimginit.h"
#include "zstitchimage.h"
#include "zimgnccmatch.h"
#include "zimgblockprovider.h"
#include "zimgsliceprovider.h"
#include "zimgmerge.h"
#include "zpunctadetection.h"
#include "zsectionsregistration.h"
#include "zchromaticshiftcorrection.h"
#include "zroimaskrasterizer.h"
#include "zimgautothreshold.h"
#include "zneutubeautotraceprocess.h"
#include "zneutubeblockedautotraceprocess.h"
#include "zneutubeskeletonizeprocess.h"
#include "zneutubetraceconfig.h"
#include "zswcsubtract.h"
#include "zswc.h"
#include "zmesh.h"
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/ndarray.h>
#include <nanobind/eigen/dense.h>
#include <nanobind/operators.h>
#include <nanobind/trampoline.h>
#include <cstring>
#include <string_view>

namespace nb = nanobind;

using namespace nim;

using namespace nb::literals;

namespace {

nanobind::dlpack::dtype getDType(const ZImg& img)
{
  if (img.voxelFormat() == VoxelFormat::Unsigned) {
    switch (img.bytesPerVoxel()) {
      case 1:
        return nanobind::dtype<uint8_t>();
      case 2:
        return nanobind::dtype<uint16_t>();
      case 4:
        return nanobind::dtype<uint32_t>();
      case 8:
        return nanobind::dtype<uint64_t>();
      default:
        throw ZException("Incorrect Img Info");
    }
  } else if (img.voxelFormat() == VoxelFormat::Float) {
    switch (img.bytesPerVoxel()) {
      case 4:
        return nanobind::dtype<float>();
      case 8:
        return nanobind::dtype<double>();
      default:
        throw ZException("Incorrect Img Info");
    }
  } else {
    switch (img.bytesPerVoxel()) {
      case 1:
        return nanobind::dtype<int8_t>();
      case 2:
        return nanobind::dtype<int16_t>();
      case 4:
        return nanobind::dtype<int32_t>();
      case 8:
        return nanobind::dtype<int64_t>();
      default:
        throw ZException("Incorrect Img Info");
    }
  }
}

[[nodiscard]] nb::builtin_exception makeTypeError(std::string message)
{
  // nanobind's builtin_exception constructors accept const char*.
  return nb::type_error(message.c_str());
}

[[nodiscard]] nb::builtin_exception makeValueError(std::string message)
{
  // nanobind's builtin_exception constructors accept const char*.
  return nb::value_error(message.c_str());
}

ZImgInfo getImgInfoFromNdarray(const nb::ndarray<>& arr, const ZImgInfo& info_in, const std::string& layout)
{
  if (arr.size() <= 0) {
    throw ZException("Empty ndarray");
  }
  ZImgInfo res = info_in;
  auto lm = zpy::parseLayout(layout, (int)arr.ndim());
  auto [c, z, y, x] = zpy::dimsFromLayout(arr, lm);
  res.numTimes = 1;
  res.numChannels = c;
  res.depth = z;
  res.height = y;
  res.width = x;
  res.bytesPerVoxel = arr.itemsize();

  switch (static_cast<nanobind::dlpack::dtype_code>(arr.dtype().code)) {
    case nanobind::dlpack::dtype_code::Int:
      res.voxelFormat = VoxelFormat::Signed;
      break;
    case nanobind::dlpack::dtype_code::UInt:
      res.voxelFormat = VoxelFormat::Unsigned;
      break;
    case nanobind::dlpack::dtype_code::Float:
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
  NB_TRAMPOLINE(ZImgSubBlockBase, 2);
  std::shared_ptr<ZImg> read() const override
  {
    NB_OVERRIDE_PURE(read);
  }
  ZImgInfo readInfo() const override
  {
    NB_OVERRIDE_PURE(readInfo);
  }
};
template<class ZImgTileSubBlockBase = ZImgTileSubBlock>
class PyZImgTileSubBlock : public ZImgTileSubBlockBase
{
public:
  NB_TRAMPOLINE(ZImgTileSubBlockBase, 2);
  std::shared_ptr<ZImg> read() const override
  {
    NB_OVERRIDE(read);
  }
  ZImgInfo readInfo() const override
  {
    NB_OVERRIDE(readInfo);
  }
};

class PyZImgSliceProvider : public ZImgSliceProvider
{
public:
  NB_TRAMPOLINE(ZImgSliceProvider, 4);

  ZImgInfo imgInfo() const override
  {
    NB_OVERRIDE_PURE(imgInfo);
  }

  ZImg slice(size_t z, size_t t) const override
  {
    NB_OVERRIDE_PURE(slice, z, t);
  }

  ZImg allSlices(size_t t) const override
  {
    NB_OVERRIDE(allSlices, t);
  }

  ZImg wholeImg() const override
  {
    NB_OVERRIDE(wholeImg);
  }
};

class PyZImgBlockProvider : public ZImgBlockProvider
{
public:
  NB_TRAMPOLINE(ZImgBlockProvider, 5);

  ZImgInfo imgInfo() const override
  {
    NB_OVERRIDE_PURE(imgInfo);
  }

  size_t numBlocks() const override
  {
    NB_OVERRIDE_PURE(numBlocks);
  }

  ZImg block(size_t blockIdx) const override
  {
    NB_OVERRIDE_PURE(block, blockIdx);
  }

  ZVoxelCoordinate blockCoord(size_t blockIdx) const override
  {
    NB_OVERRIDE_PURE(blockCoord, blockIdx);
  }

  ZImg wholeImg() const override
  {
    NB_OVERRIDE(wholeImg);
  }
};

template<size_t L, typename T>
std::vector<glm::vec<L, T>> arrayToVecVec(const nb::ndarray<T>& array)
{
  if (array.ndim() != 2 || array.shape(1) != L) {
    throw ZException(fmt::format("input array shape does not match output glm::vec{}", L));
  }
  std::vector<glm::vec<L, T>> res;
  if (array.shape(0) > 0) {
    res.resize(array.shape(0));
    auto* data = array.data();
    int64_t s0 = array.stride(0);
    int64_t s1 = array.stride(1);
    if (s1 == 1 && s0 == (int64_t)L) {
      std::memcpy(res.data(), data, sizeof(T) * L * res.size());
    } else {
      for (size_t i = 0; i < res.size(); ++i) {
        for (size_t j = 0; j < L; ++j) {
          res[i][j] = *(data + i * s0 + j * s1);
        }
      }
    }
  }
  return res;
}

template<size_t L, typename T>
nb::ndarray<nanobind::numpy, const T> vecVecToArray(const std::vector<glm::vec<L, T>>& v)
{
  return v.empty() ? nb::ndarray<nanobind::numpy, const T>()
                   : nb::ndarray<nanobind::numpy, const T>(&v[0].x, {v.size(), L});
}

template<typename T>
std::vector<T> arrayToVector(const nb::ndarray<T>& array)
{
  if (array.ndim() != 1) {
    throw ZException(fmt::format("input array shape does not match output"));
  }
  std::vector<T> res;
  if (array.shape(0) > 0) {
    res.resize(array.shape(0));
    auto* data = array.data();
    int64_t s0 = array.stride(0);
    if (s0 == 1) {
      std::memcpy(res.data(), data, sizeof(T) * res.size());
    } else {
      for (size_t i = 0; i < res.size(); ++i) {
        res[i] = *(data + i * s0);
      }
    }
  }
  return res;
}

template<typename T>
nb::ndarray<nanobind::numpy, const T> vectorToArray(const std::vector<T>& v)
{
  return v.empty() ? nb::ndarray<nanobind::numpy, const T>()
                   : nb::ndarray<nanobind::numpy, const T>(v.data(), {v.size()});
}

std::vector<glm::dvec2> roiPointsFromArray(const nb::ndarray<nb::numpy, const double>& points, std::string_view name)
{
  if (points.ndim() != 2 || points.shape(1) != 2) {
    throw makeTypeError(fmt::format("{} must have shape (N, 2)", name));
  }

  std::vector<glm::dvec2> out;
  out.reserve(points.shape(0));

  const auto* data = points.data();
  const int64_t s0 = points.stride(0);
  const int64_t s1 = points.stride(1);

  for (size_t i = 0; i < points.shape(0); ++i) {
    const int64_t base = static_cast<int64_t>(i) * s0;
    const double x = *(data + base + 0 * s1);
    const double y = *(data + base + 1 * s1);
    out.emplace_back(x, y);
  }
  return out;
}

ZROIMaskShapeType roiShapeTypeFromString(const std::string& type)
{
  if (type == "Rect") {
    return ZROIMaskShapeType::Rect;
  }
  if (type == "Ellipse") {
    return ZROIMaskShapeType::Ellipse;
  }
  if (type == "Polygon") {
    return ZROIMaskShapeType::Polygon;
  }
  if (type == "Spline") {
    return ZROIMaskShapeType::Spline;
  }
  if (type == "Line") {
    return ZROIMaskShapeType::Line;
  }
  throw makeValueError(fmt::format("Unsupported ROI shape type: {}", type));
}

void validateROIPoints(const std::vector<glm::dvec2>& points, ZROIMaskShapeType type, std::string_view ctx)
{
  if (type == ZROIMaskShapeType::Rect || type == ZROIMaskShapeType::Ellipse) {
    if (points.size() != 2) {
      throw makeValueError(fmt::format("{} requires exactly 2 points", ctx));
    }
    return;
  }

  if (type == ZROIMaskShapeType::Polygon) {
    if (points.size() < 4 || points.front() != points.back()) {
      throw makeValueError(fmt::format("{} must be closed (first point == last point)", ctx));
    }
    return;
  }

  if (type == ZROIMaskShapeType::Spline || type == ZROIMaskShapeType::Line) {
    if (points.size() < 2) {
      throw makeValueError(fmt::format("{} requires at least 2 points", ctx));
    }
    return;
  }

  throw nb::value_error("Unknown ZROIMaskShapeType");
}

json::value pythonObjectToJsonValue(nb::handle obj)
{
  PyObject* ptr = obj.ptr();

  if (ptr == Py_None) {
    return nullptr;
  }
  if (PyBool_Check(ptr)) {
    return json::value_from(ptr == Py_True);
  }
  if (PyLong_Check(ptr)) {
    int overflow = 0;
    const long long signedValue = PyLong_AsLongLongAndOverflow(ptr, &overflow);
    if (!PyErr_Occurred() && overflow == 0) {
      return json::value_from(static_cast<int64_t>(signedValue));
    }
    PyErr_Clear();

    const unsigned long long unsignedValue = PyLong_AsUnsignedLongLong(ptr);
    if (!PyErr_Occurred()) {
      return json::value_from(static_cast<uint64_t>(unsignedValue));
    }
    PyErr_Clear();
    throw nb::value_error("Python int is outside the supported JSON integer range");
  }
  if (PyFloat_Check(ptr)) {
    return json::value_from(nb::cast<double>(obj));
  }
  if (PyUnicode_Check(ptr)) {
    return json::value_from(nb::cast<std::string>(obj));
  }
  if (PyDict_Check(ptr)) {
    json::object out;
    for (auto item : nb::borrow<nb::dict>(obj)) {
      if (!PyUnicode_Check(item.first.ptr())) {
        throw nb::type_error("JSON object keys must be strings");
      }
      out[nb::cast<std::string>(item.first)] = pythonObjectToJsonValue(item.second);
    }
    return out;
  }
  if (PyList_Check(ptr) || PyTuple_Check(ptr)) {
    json::array out;
    for (nb::handle item : nb::borrow<nb::sequence>(obj)) {
      out.push_back(pythonObjectToJsonValue(item));
    }
    return out;
  }

  throw nb::type_error("Expected a JSON-compatible Python object");
}

json::object pythonObjectToJsonObject(nb::handle obj, std::string_view name)
{
  json::value value = pythonObjectToJsonValue(obj);
  if (!value.is_object()) {
    throw makeTypeError(fmt::format("{} must be a dict-like JSON object", name));
  }
  return value.as_object();
}

template<typename T>
std::array<T, 3> vectorToArray3(const std::vector<T>& values, std::string_view name)
{
  if (values.size() != 3) {
    throw makeValueError(fmt::format("{} must contain exactly 3 elements", name));
  }
  return {values[0], values[1], values[2]};
}

void initializeRuntime(const QString& resourcesDir, const QString& jarsDir, const QString& javaExecutablePath)
{
  ZLogInit::instance("zimg"s);
  ZImgInit::instance(resourcesDir, QString(), jarsDir, false);
  if (!javaExecutablePath.isEmpty()) {
    ZBioFormatsBridgeClient::configureJavaExecutablePath(javaExecutablePath);
  }
}

nb::dict bioFormatsRuntimePaths()
{
  nb::dict result;
  result["java_executable"] = ZBioFormatsBridgeClient::javaExecutablePath().toStdString();
  result["bridge_jar"] = ZBioFormatsBridgeClient::bridgeJarPath().toStdString();
  result["bioformats_jar"] = ZBioFormatsBridgeClient::bioFormatsJarPath().toStdString();
  return result;
}

} // namespace

NB_MODULE(_imgpy, m)
{
  m.doc() = R"pbdoc(
        Python interface to img lib.
    )pbdoc";

  m.def("_initialize_runtime", &initializeRuntime, "resources_dir"_a, "jars_dir"_a, "java_executable_path"_a);
  m.def("_shutdown_runtime", &ZImgInit::shutdown);
  m.def("_set_bioformats_jar_path", &ZBioFormatsBridgeClient::configureBioFormatsJarPath, "bioformats_jar_path"_a);
  m.def("_set_bioformats_java_executable_path",
        &ZBioFormatsBridgeClient::configureJavaExecutablePath,
        "java_executable_path"_a);
  m.def("_has_bioformats_runtime_support", &ZBioFormatsBridgeClient::hasRuntimeSupport);
  m.def("_missing_bioformats_runtime_files", &ZBioFormatsBridgeClient::missingRuntimeFiles);
  m.def("_bioformats_runtime_paths", &bioFormatsRuntimePaths);

  nb::enum_<VoxelFormat>(m, "VoxelFormat", nb::is_arithmetic())
    .value("Unsigned", VoxelFormat::Unsigned)
    .value("Signed", VoxelFormat::Signed)
    .value("Float", VoxelFormat::Float);

  nb::enum_<VoxelSizeUnit>(m, "VoxelSizeUnit", nb::is_arithmetic())
    .value("none", VoxelSizeUnit::none)
    .value("inch", VoxelSizeUnit::inch)
    .value("cm", VoxelSizeUnit::cm)
    .value("mm", VoxelSizeUnit::mm)
    .value("um", VoxelSizeUnit::um)
    .value("nm", VoxelSizeUnit::nm)
    .value("m", VoxelSizeUnit::m)
    .value("hm", VoxelSizeUnit::hm)
    .value("km", VoxelSizeUnit::km);

  nb::enum_<Interpolant>(m, "Interpolant", nb::is_arithmetic())
    .value("Nearest", Interpolant::Nearest)
    .value("Linear", Interpolant::Linear)
    .value("Cubic", Interpolant::Cubic)
    .value("Lanczos2", Interpolant::Lanczos2)
    .value("Lanczos3", Interpolant::Lanczos3);

  nb::enum_<Dimension>(m, "Dimension", nb::is_arithmetic())
    .value("X", Dimension::X)
    .value("Y", Dimension::Y)
    .value("Z", Dimension::Z)
    .value("C", Dimension::C)
    .value("T", Dimension::T);

  nb::enum_<ImgMergeMode>(m, "ImgMergeMode")
    .value("Max", ImgMergeMode::Max)
    .value("Min", ImgMergeMode::Min)
    .value("Mean", ImgMergeMode::Mean)
    .value("Median", ImgMergeMode::Median)
    .value("First", ImgMergeMode::First)
    .value("Interpolation", ImgMergeMode::Interpolation);

  nb::enum_<FileFormat>(m, "FileFormat", nb::is_arithmetic())
    .value("Unknown", FileFormat::Unknown)
    .value("HDF5Img", FileFormat::HDF5Img)
    .value("OmeTiff", FileFormat::OmeTiff)
    .value("Tiff", FileFormat::Tiff)
    .value("Vaa3DRaw", FileFormat::Vaa3DRaw)
    .value("ZeissLsm", FileFormat::ZeissLsm)
    .value("Jpeg", FileFormat::Jpeg)
    .value("JpegXR", FileFormat::JpegXR)
    .value("Png", FileFormat::Png)
    .value("MetaImage", FileFormat::MetaImage)
    .value("ZeissCZI", FileFormat::ZeissCZI)
    .value("ITKImage", FileFormat::ITKImage)
    .value("Leica", FileFormat::Leica)
    .value("OpenImageIO", FileFormat::OpenImageIO)
    .value("BioFormats", FileFormat::BioFormats);

  nb::enum_<Compression>(m, "Compression", nb::is_arithmetic())
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

  nb::class_<col4>(m,
                   "col4",
                   "This class represents a color in RGBA format where each channel is an 8-bit unsigned integer.")
    .def(nb::init<>(), "Default constructor that initializes the color to black with full opacity.")
    .def(nb::init<uint8_t, uint8_t, uint8_t, uint8_t>(),
         "Constructor that initializes the color with given RGBA values.",
         "r"_a,
         "g"_a,
         "b"_a,
         "a"_a = 255)
    .def_rw("r", &col4::r, "Red channel value of the color. It ranges from 0 to 255.")
    .def_rw("g", &col4::g, "Green channel value of the color. It ranges from 0 to 255.")
    .def_rw("b", &col4::b, "Blue channel value of the color. It ranges from 0 to 255.")
    .def_rw(
      "a",
      &col4::a,
      "Alpha channel value of the color. It represents opacity and ranges from 0 (fully transparent) to 255 (fully opaque).")
    .def(
      "__init__",
      [](col4& self, nb::tuple t) {
        if (nb::len(t) != 4) {
          throw ZException("col4 needs tuple with 4 values");
        }
        new (&self)
          col4{nb::cast<uint8_t>(t[0]), nb::cast<uint8_t>(t[1]), nb::cast<uint8_t>(t[2]), nb::cast<uint8_t>(t[3])};
      },
      "Constructor that initializes the color with a tuple of four 8-bit unsigned integers representing RGBA color channels respectively.")
    .def(
      "__repr__",
      [](const col4& v) {
        return fmt::format("<_imgpy.col4 rgba:{}>", v);
      },
      "Returns a string representation of the color.");
  nb::implicitly_convertible<nb::tuple, col4>();

  auto zException = nb::exception<ZException>(m, "ZException", PyExc_RuntimeError);
  nb::exception<ZCancellationException>(m, "ZCancellationException", zException);

  nb::class_<ZImgWriteParameters>(m,
                                  "ZImgWriteParameters",
                                  "This class holds different parameters for configuring image compression.")
    .def(nb::init<>(), "Default constructor. Initializes the parameters with their respective default values.")
    .def_rw(
      "compression",
      &ZImgWriteParameters::compression,
      "Specifies the compression algorithm to use, default to Compression.Auto. For NIM/HDF5 writes, Auto uses Zstd.")
    .def_rw(
      "zlibCompressionLevel",
      &ZImgWriteParameters::zlibCompressionLevel,
      "Specifies the compression level for the zlib algorithm. Ranges from 1 (fastest compression) to 9 (best compression), default to 6.")
    .def_rw(
      "jpegQuality",
      &ZImgWriteParameters::jpegQuality,
      "Specifies the quality factor for the JPEG compression. Ranges from 0 (lowest quality) to 100 (highest quality), default to 95.")
    .def_rw("jpegProgressive",
            &ZImgWriteParameters::jpegProgressive,
            "If set to True (default), creates a progressive JPEG file. If False, creates a baseline JPEG file.")
    .def_rw(
      "jpegAccurateDCT",
      &ZImgWriteParameters::jpegAccurateDCT,
      "If set to True (default), uses the accurate Discrete Cosine Transform (DCT) method. If False, uses the fast DCT method.")
    .def_rw(
      "jpegChrominanceSubsampling",
      &ZImgWriteParameters::jpegChrominanceSubsampling,
      "Specifies the chrominance subsampling scheme to use for the JPEG compression, 444 (default, no subsampling) or 422 or 420, only apply to RGB")
    .def_rw(
      "jpegXRQuality",
      &ZImgWriteParameters::jpegXRQuality,
      "Specifies the quality factor for the JPEG XR compression. Ranges [0.0 - 1.0], 1.0 is lossless, default to 0.8.")
    .def_rw("zstdCompressionLevel",
            &ZImgWriteParameters::zstdCompressionLevel,
            "Specifies the compression level for Zstd. Negative values favor speed over density, default to 1.");

  nb::class_<ZImgInfo>(m, "ZImgInfo", "This class holds the metadata for a multidimensional image.")
    .def(nb::init<>(), "Default constructor that initializes the image information with default values.")
    .def(nb::init<size_t, size_t, size_t, size_t, size_t, size_t, VoxelFormat>(),
         "Constructor that initializes the image information with given values.",
         "width"_a,
         "height"_a,
         "depth"_a = 1,
         "numChannels"_a = 1,
         "numTimes"_a = 1,
         "bytePerVox"_a = 1,
         "voxelFormat"_a = VoxelFormat::Unsigned)
    .def_rw("width", &ZImgInfo::width, "Width of the image.")
    .def_rw("height", &ZImgInfo::height, "Height of the image.")
    .def_rw("depth", &ZImgInfo::depth, "Depth of the image.")
    .def_rw("numChannels", &ZImgInfo::numChannels, "Number of color channels in the image.")
    .def_rw("numTimes", &ZImgInfo::numTimes, "Number of time points in the image sequence.")
    .def_rw("bytesPerVoxel", &ZImgInfo::bytesPerVoxel, "Size of each voxel in bytes.")
    .def_rw("voxelFormat", &ZImgInfo::voxelFormat, "Format of the voxels.")
    .def_rw("validBitCount", &ZImgInfo::validBitCount, "Number of valid bits in each voxel.")
    .def_rw("voxelSizeUnit", &ZImgInfo::voxelSizeUnit, "Unit of the voxel size.")
    .def_rw("voxelSizeX", &ZImgInfo::voxelSizeX, "Size of the voxel in the x dimension.")
    .def_rw("voxelSizeY", &ZImgInfo::voxelSizeY, "Size of the voxel in the y dimension.")
    .def_rw("voxelSizeZ", &ZImgInfo::voxelSizeZ, "Size of the voxel in the z dimension.")
    .def_rw("timeStamps", &ZImgInfo::timeStamps, "Timestamps for each frame in the image sequence.")
    .def_rw("channelNames", &ZImgInfo::channelNames, "Names of the color channels.")
    .def_rw("channelColors", &ZImgInfo::channelColors, "Color of each channel.")
    .def_rw("position", &ZImgInfo::position, "Position of the image in a larger sequence or collection.")
    .def_rw("lastChannelIsAlphaChannel",
            &ZImgInfo::lastChannelIsAlphaChannel,
            "Boolean value indicating if the last channel is the alpha channel.")
    .def("voxelSizeXInUm",
         &ZImgInfo::voxelSizeXInUm,
         "Returns the size of the voxel in the x dimension in micrometers.")
    .def("voxelSizeYInUm",
         &ZImgInfo::voxelSizeYInUm,
         "Returns the size of the voxel in the y dimension in micrometers.")
    .def("voxelSizeZInUm",
         &ZImgInfo::voxelSizeZInUm,
         "Returns the size of the voxel in the z dimension in micrometers.")
    .def("dataTypeString", &ZImgInfo::typeAsQString, "Returns the data type of the voxels as a string.")
    .def(
      "__repr__",
      [](const ZImgInfo& v) {
        return fmt::format("<_imgpy.ZImgInfo {}>", v.toString());
      },
      "Returns a string representation of the image information.");

  nb::class_<ZVoxelCoordinate>(m,
                               "ZVoxelCoordinate",
                               "This class represents the 5D coordinates of a voxel in an image.")
    .def(nb::init<>(), "Default constructor that initializes the voxel coordinates to zero.")
    .def(nb::init<ZVoxelCoordinate::value_type,
                  ZVoxelCoordinate::value_type,
                  ZVoxelCoordinate::value_type,
                  ZVoxelCoordinate::value_type,
                  ZVoxelCoordinate::value_type>(),
         "Constructor that initializes the voxel coordinates with the provided values.",
         "x"_a,
         "y"_a,
         "z"_a = 0,
         "c"_a = 0,
         "t"_a = 0)
    .def_rw("x", &ZVoxelCoordinate::x, "x-coordinate of the voxel.")
    .def_rw("y", &ZVoxelCoordinate::y, "y-coordinate of the voxel.")
    .def_rw("z", &ZVoxelCoordinate::z, "z-coordinate of the voxel.")
    .def_rw("c", &ZVoxelCoordinate::c, "c-coordinate of the voxel.")
    .def_rw("t", &ZVoxelCoordinate::t, "t-coordinate of the voxel.")
    .def(
      "__init__",
      [](ZVoxelCoordinate& self, nb::tuple t) {
        if (nb::len(t) != 5) {
          throw ZException("ZVoxelCoordinate needs tuple with 5 values");
        }
        new (&self) ZVoxelCoordinate(nb::cast<ZVoxelCoordinate::value_type>(t[0]),
                                     nb::cast<ZVoxelCoordinate::value_type>(t[1]),
                                     nb::cast<ZVoxelCoordinate::value_type>(t[2]),
                                     nb::cast<ZVoxelCoordinate::value_type>(t[3]),
                                     nb::cast<ZVoxelCoordinate::value_type>(t[4]));
      },
      "Constructor that initializes the voxel coordinates from a tuple of 5 values.")
    .def(
      "__repr__",
      [](const ZVoxelCoordinate& v) {
        return fmt::format("<_imgpy.ZVoxelCoordinate xyzct:{}>", v);
      },
      "Returns a string representation of the voxel coordinates.");
  nb::implicitly_convertible<nb::tuple, ZVoxelCoordinate>();

  nb::class_<ZImgRegion>(
    m,
    "ZImgRegion",
    "This class represents a region in an image defined by start and end (not included) voxel coordinates.")
    .def(
      nb::init<>(),
      "Default constructor that initializes the start voxel coordinates to zero, and end voxel coordinates to -1, which means to the end of that dimension.")
    .def(nb::init<ZVoxelCoordinate, ZVoxelCoordinate>(),
         "Constructor that initializes the region with the provided start and end (not included) voxel coordinates.",
         "start"_a,
         "end"_a)
    .def_rw("start", &ZImgRegion::start, "Start voxel coordinate of the image region.")
    .def_rw("end", &ZImgRegion::end, "End (not included) voxel coordinate of the image region.")
    .def(
      "__repr__",
      [](const ZImgRegion& v) {
        return fmt::format("<_imgpy.ZImgRegion {}>", v.toString());
      },
      "Returns a string representation of the image region.");

  nb::class_<ZImgSource>(m, "ZImgSource", "This class represents the source of an image or a collection of images.")
    .def(nb::init<>(), "Default constructor that initializes an empty image source.")
    .def(nb::init<const QString&, const ZImgRegion&, size_t, FileFormat>(),
         "Constructor that initializes the image source with a single file.",
         "filename"_a,
         "region"_a = ZImgRegion(),
         "scene"_a = 0,
         "format"_a = FileFormat::Unknown)
    .def(nb::init<const QStringList&, Dimension, bool, const ZImgRegion&, size_t, FileFormat, bool, bool>(),
         "Constructor that initializes the image source with multiple files.",
         "filenames"_a,
         "catDim"_a,
         "catScenes"_a = true,
         "region"_a = ZImgRegion(),
         "scene"_a = 0,
         "format"_a = FileFormat::Unknown,
         "expandXY"_a = true,
         "expandWithMaxValue"_a = false)
    .def_rw("filenames", &ZImgSource::filenames, "List of filenames in the image source.")
    .def_rw("catDim", &ZImgSource::catDim, "Dimension to concatenate when multiple files are given.")
    .def_rw("region", &ZImgSource::region, "Region of interest in the image.")
    .def_rw("scene", &ZImgSource::scene, "Scene index for formats that support multiple scenes.")
    .def_rw("format", &ZImgSource::format, "Format of the image file(s).")
    .def_rw("expandXY", &ZImgSource::expandXY, "Flag to indicate if the XY dimensions should be expanded.")
    .def_rw("expandWithMaxValue",
            &ZImgSource::expandWithMaxValue,
            "Flag to indicate if expansion should be done with max value.")
    .def_rw("totalFileSize", &ZImgSource::totalFileSize, "Total file size of the image source.")
    .def_rw("catScenes", &ZImgSource::catScenes, "Flag to indicate if scenes should be concatenated.")
    .def(
      "__repr__",
      [](const ZImgSource& v) {
        return fmt::format("<_imgpy.ZImgSource {}>", v.toString());
      },
      "Returns a string representation of the image source.");

  nb::class_<ZImg>(m, "ZImg", nb::dynamic_attr(), R"doc(
ZImg — multidimensional image container with fast CPU array interop.

Overview
- Read/process 2D/3D/CT images from single files, file lists, or ZImgSource.
- CPU array interop via nb::ndarray: wrap (zero‑copy) or copy depending on
  layout/contiguity and the 'copy_if_needed' flag in ndarray constructors.
- Returned arrays keep the parent alive (reference_internal). Zero‑copy inputs
  keep original owners attached to the instance.

Array interop summary
- Zero‑copy when layout='CZYX' and the array is CPU C‑contiguous; otherwise
  copies unless copy_if_needed=False (then raises).
- to_arrays(framework='auto') returns CPU‑backed arrays in 'numpy', 'torch',
  'tensorflow', 'jax', 'array_api', or 'memview'. 'auto' mirrors the input
  framework if created zero‑copy; otherwise returns NumPy.
- Inputs/outputs are CPU‑backed only.

See also
- __init__(ndarray, info=..., copy_if_needed=True, layout='CZYX')
- __init__(list[ndarray], info=..., copy_if_needed=True, layout='CZYX')
- data, to_arrays(framework='auto')
- readImgInfos(), readImgInfo(), readImgPixelsOnly(), readSubBlockLists(), readSubBlock(), save(), writeImg()
)doc")
    .def(nb::init<>())
    .def(nb::init<const ZImgInfo&>())
    .def(nb::init<const QString&, ZImgRegion, size_t, size_t, size_t, size_t, FileFormat>(),
         "filename"_a,
         "region"_a = ZImgRegion(),
         "scene"_a = 0,
         "xRatio"_a = 1,
         "yRatio"_a = 1,
         "zRatio"_a = 1,
         "format"_a = FileFormat::Unknown,
         nb::call_guard<nb::gil_scoped_release>())
    .def(nb::init<const QStringList&,
                  Dimension,
                  bool,
                  const ZImgRegion&,
                  size_t,
                  size_t,
                  size_t,
                  size_t,
                  FileFormat,
                  bool,
                  bool>(),
         "filenames"_a,
         "catDim"_a,
         "catScenes"_a,
         "region"_a = ZImgRegion(),
         "scene"_a = 0,
         "xRatio"_a = 1,
         "yRatio"_a = 1,
         "zRatio"_a = 1,
         "format"_a = FileFormat::Unknown,
         "expandXY"_a = true,
         "expandWithMaxValue"_a = false,
         nb::call_guard<nb::gil_scoped_release>())
    .def(nb::init<const ZImgSource&, size_t, size_t, size_t>(),
         "imgSource"_a,
         "xRatio"_a = 1,
         "yRatio"_a = 1,
         "zRatio"_a = 1,
         nb::call_guard<nb::gil_scoped_release>())
    .def(
      "__init__",
      [](nb::pointer_and_handle<ZImg> v,
         const nb::ndarray<>& arr,
         const ZImgInfo& info_in,
         bool copy_if_needed,
         std::string layout) {
        if (!zpy::isCPU(arr)) {
          throw ZException("GPU-backed nb::ndarray is not supported; please move data to CPU or pass a CPU array");
        }
        auto info = getImgInfoFromNdarray(arr, info_in, layout);
        if (layout != "CZYX" || !zpy::isCPU(arr) || !zpy::isContiguousC(arr)) {
          if (!copy_if_needed) {
            throw ZException("Input ndarray not CPU C-contiguous in CZYX; set copy_if_needed=True or adjust layout");
          }
          // Allocate and copy into ZImg-owned storage
          new ((void*)v.p) ZImg(info);
          auto item = arr.itemsize();
          // Strides in bytes for source
          auto lm = zpy::parseLayout(layout, (int)arr.ndim());
          auto [sc, sz, sy, sx] = zpy::stridesFromLayout(arr, lm);
          auto* base = (const uint8_t*)arr.data();
          for (size_t cc = 0; cc < info.numChannels; ++cc) {
            for (size_t zz = 0; zz < info.depth; ++zz) {
              for (size_t yy = 0; yy < info.height; ++yy) {
                for (size_t xx = 0; xx < info.width; ++xx) {
                  const uint8_t* sp = base + (lm.c >= 0 ? (int64_t)cc * sc : 0) + (lm.z >= 0 ? (int64_t)zz * sz : 0) +
                                      (int64_t)yy * sy + (int64_t)xx * sx;
                  uint8_t* dp = (uint8_t*)v.p->timeData(0) + cc * (int64_t)info.channelByteNumber() +
                                zz * (int64_t)info.planeByteNumber() + yy * (int64_t)info.rowByteNumber() +
                                xx * (int64_t)info.voxelByteNumber();
                  std::memcpy(dp, sp, item);
                }
              }
            }
          }
        } else {
          // Zero-copy, keep owner alive on the Python object
          new ((void*)v.p) ZImg();
          v.p->wrapData(const_cast<void*>(static_cast<const void*>(arr.data())), info);
          nb::list owners;
          owners.append(arr);
          v.h.attr("_owners") = owners;
        }
      },
      "ndarray"_a,
      "imgInfo"_a = ZImgInfo(),
      "copy_if_needed"_a = true,
      "layout"_a = std::string("CZYX"))
    .def(
      "__init__",
      [](nb::pointer_and_handle<ZImg> v,
         const std::vector<nb::ndarray<>>& arrs,
         const ZImgInfo& info_in,
         bool copy_if_needed,
         std::string layout) {
        if (arrs.empty()) {
          new ((void*)v.p) ZImg();
          return;
        }
        for (auto& a : arrs) {
          if (!zpy::isCPU(a)) {
            throw ZException("GPU-backed nb::ndarray is not supported; please move data to CPU or pass CPU arrays");
          }
        }
        auto info = getImgInfoFromNdarray(arrs[0], info_in, layout);
        // Validate all arrays compatible
        for (size_t t = 1; t < arrs.size(); ++t) {
          auto tmp = getImgInfoFromNdarray(arrs[t], info_in, layout);
          if (!tmp.isSameType(info) || !tmp.isSameSize(info)) {
            throw ZException("ndarrays in the list are not compatible");
          }
        }
        info.numTimes = arrs.size();
        bool zerocopy = (layout == "CZYX");
        for (auto& a : arrs) {
          zerocopy = zerocopy && zpy::isCPU(a) && zpy::isContiguousC(a);
        }
        if (!zerocopy) {
          if (!copy_if_needed) {
            throw ZException("Input arrays not CPU C-contiguous in CZYX; set copy_if_needed=True or adjust layout");
          }
          // Allocate and copy into ZImg-owned storage
          new ((void*)v.p) ZImg(info);
          auto item = arrs[0].itemsize();
          for (size_t t = 0; t < arrs.size(); ++t) {
            auto& arr = arrs[t];
            auto lm = zpy::parseLayout(layout, (int)arr.ndim());
            auto [sc, sz, sy, sx] = zpy::stridesFromLayout(arr, lm);
            auto* base = (const uint8_t*)arr.data();
            for (size_t cc = 0; cc < info.numChannels; ++cc) {
              for (size_t zz = 0; zz < info.depth; ++zz) {
                for (size_t yy = 0; yy < info.height; ++yy) {
                  for (size_t xx = 0; xx < info.width; ++xx) {
                    const uint8_t* sp = base + (lm.c >= 0 ? (int64_t)cc * sc : 0) + (lm.z >= 0 ? (int64_t)zz * sz : 0) +
                                        (int64_t)yy * sy + (int64_t)xx * sx;
                    uint8_t* dp = (uint8_t*)v.p->timeData(t) + cc * (int64_t)info.channelByteNumber() +
                                  zz * (int64_t)info.planeByteNumber() + yy * (int64_t)info.rowByteNumber() +
                                  xx * (int64_t)info.voxelByteNumber();
                    std::memcpy(dp, sp, item);
                  }
                }
              }
            }
          }
        } else {
          // Zero-copy: wrap pointers and keep owners
          new ((void*)v.p) ZImg();
          std::vector<void*> data;
          for (auto& a : arrs) {
            data.push_back(const_cast<void*>(static_cast<const void*>(a.data())));
          }
          v.p->wrapData(data, info);
          nb::list owners;
          for (auto& a : arrs) {
            owners.append(a);
          }
          v.h.attr("_owners") = owners;
        }
      },
      "listOfndarray"_a,
      "imgInfo"_a = ZImgInfo(),
      "copy_if_needed"_a = true,
      "layout"_a = std::string("CZYX"))
    .def_static(
      "readImgInfos",
      [](const QString& filename, FileFormat format) {
        return ZImg::readImgInfos(filename, nullptr, format);
      },
      "filename"_a,
      "format"_a = FileFormat::Unknown,
      "Read image info from a single file. Optionally specify the file format.")
    .def_static(
      "readImgInfos",
      [](const QStringList& fileList, Dimension catDim, bool catScenes, FileFormat format, bool expandXY) {
        return ZImg::readImgInfos(fileList, catDim, catScenes, nullptr, format, expandXY);
      },
      "filenames"_a,
      "catDim"_a,
      "catScenes"_a,
      "format"_a = FileFormat::Unknown,
      "expandXY"_a = true,
      "Read image info from a list of files, concatenating along 'catDim'."
      " If 'catScenes' is true, scenes are concatenated as well. 'expandXY'"
      " controls expansion of XY dimensions where needed.")
    .def_static(
      "readImgInfo",
      [](const ZImgSource& imgSource) {
        return ZImg::readImgInfo(imgSource);
      },
      "Read image info from a ZImgSource descriptor.")
    .def_static(
      "readImgPixelsOnly",
      [](const QString& filename,
         const ZImgRegion& region,
         size_t scene,
         size_t xRatio,
         size_t yRatio,
         size_t zRatio,
         FileFormat format) {
        return ZImg::readImgPixelsOnly(filename, region, scene, xRatio, yRatio, zRatio, format);
      },
      "filename"_a,
      "region"_a = ZImgRegion(),
      "scene"_a = 0,
      "xRatio"_a = 1,
      "yRatio"_a = 1,
      "zRatio"_a = 1,
      "format"_a = FileFormat::Unknown,
      nb::call_guard<nb::gil_scoped_release>(),
      "Read image pixels without attaching descriptive metadata or thumbnails.")
    .def_static(
      "readImgPixelsOnly",
      [](const ZImgSource& imgSource, size_t xRatio, size_t yRatio, size_t zRatio) {
        return ZImg::readImgPixelsOnly(imgSource, xRatio, yRatio, zRatio);
      },
      "imgSource"_a,
      "xRatio"_a = 1,
      "yRatio"_a = 1,
      "zRatio"_a = 1,
      nb::call_guard<nb::gil_scoped_release>(),
      "Read image pixels without attaching descriptive metadata or thumbnails.")
    .def_static(
      "readSubBlockLists",
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
      },
      "filename"_a,
      "format"_a = FileFormat::Unknown,
      "Read sub-block lists from a single file. Returns a list of (t, x, y, z, width, height, depth, xRatio, yRatio, zRatio) per scene.")
    .def_static(
      "readSubBlockLists",
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
      },
      "filenames"_a,
      "catDim"_a,
      "catScenes"_a,
      "format"_a = FileFormat::Unknown,
      "expandXY"_a = true,
      "Read sub-block lists from a list of files with concatenation options. Return format matches the single-file variant.")
    .def_static(
      "readSubBlock",
      [](const QString& filename, size_t scene, size_t blockIndex, FileFormat format) {
        return ZImg::readSubBlock(filename, scene, blockIndex, format);
      },
      "filename"_a,
      "scene"_a,
      "blockIndex"_a,
      "format"_a = FileFormat::Unknown,
      nb::call_guard<nb::gil_scoped_release>(),
      "Read an individual sub-block from a single file by scene and block index.")
    .def_static(
      "readSubBlock",
      [](const QStringList& fileList,
         Dimension catDim,
         bool catScenes,
         size_t scene,
         size_t blockIndex,
         FileFormat format,
         bool expandXY) {
        return ZImg::readSubBlock(fileList, catDim, catScenes, scene, blockIndex, format, expandXY);
      },
      "filenames"_a,
      "catDim"_a,
      "catScenes"_a,
      "scene"_a,
      "blockIndex"_a,
      "format"_a = FileFormat::Unknown,
      "expandXY"_a = true,
      nb::call_guard<nb::gil_scoped_release>(),
      "Read an individual sub-block from a list of files with concatenation options.")
    .def_static(
      "getInternalSubRegions",
      [](const QString& filename, FileFormat format) {
        return ZImg::getInternalSubRegions(filename, format);
      },
      "filename"_a,
      "format"_a = FileFormat::Unknown,
      "Return a list of internal sub-regions described by the file (if any).")
    .def("isEmpty", &ZImg::isEmpty)
    .def("save", &ZImg::save, "filename"_a, "format"_a = FileFormat::Unknown, "paras"_a = ZImgWriteParameters())
    .def("save",
         &ZImg::save,
         "filename"_a,
         "format"_a = FileFormat::Unknown,
         "paras"_a = ZImgWriteParameters(),
         "Save this image to 'filename' in the specified format using the given write parameters.")
    .def_static("writeImg",
                nb::overload_cast<const QString&, const ZImgSliceProvider&, FileFormat, const ZImgWriteParameters&>(
                  &ZImg::writeImg),
                "filename"_a,
                "sliceProvider"_a,
                "format"_a = FileFormat::Unknown,
                "paras"_a = ZImgWriteParameters(),
                "Write an image to 'filename' using a ZImgSliceProvider (slice-by-slice).")
    .def_static("writeImg",
                nb::overload_cast<const QString&, const ZImgBlockProvider&, FileFormat, const ZImgWriteParameters&>(
                  &ZImg::writeImg),
                "filename"_a,
                "blockProvider"_a,
                "format"_a = FileFormat::Unknown,
                "paras"_a = ZImgWriteParameters(),
                "Write an image to 'filename' using a ZImgBlockProvider (block-by-block).")
    .def_prop_rw(
      "info",
      [](const ZImg& v) {
        return v.info();
      },
      [](ZImg& v, const ZImgInfo& info) {
        v.infoRef() = info;
      })
    .def_prop_ro("data",
                 [](ZImg& v) {
                   nb::list out;
                   if (!v.isEmpty()) {
                     auto dtype = getDType(v);
                     nb::object self = nb::cast(v, nb::rv_policy::reference);
                     for (size_t t = 0; t < v.numTimes(); ++t) {
                       nb::ndarray<nb::numpy> arr(v.timeData(t),
                                                  {v.numChannels(), v.depth(), v.height(), v.width()},
                                                  nb::handle(),
                                                  {},
                                                  dtype);
                       out.append(arr.cast(nb::rv_policy::reference_internal, self));
                     }
                   }
                   return out;
                 })
    .def(
      "to_arrays",
      [](ZImg& v, std::string framework) {
        nb::list out;
        if (v.isEmpty()) {
          return out;
        }
        auto dtype = getDType(v);
        auto shape = std::initializer_list<size_t>{v.numChannels(), v.depth(), v.height(), v.width()};
        nb::object self = nb::cast(v, nb::rv_policy::reference);
        if (framework == "auto") {
          try {
            nb::object owners = self.attr("_owners");
            if (owners.is_valid() && nb::len(owners) > 0) {
              nb::object o0 = owners[0];
              std::string mod = nb::cast<std::string>(o0.attr("__class__").attr("__module__"));
              if (mod.find("torch") != std::string::npos) {
                framework = "torch";
              } else if (mod.find("tensorflow") != std::string::npos) {
                framework = "tensorflow";
              } else if (mod.find("jax") != std::string::npos) {
                framework = "jax";
              } else {
                framework = "numpy";
              }
            } else {
              framework = "numpy";
            }
          }
          catch (...) {
            framework = "numpy";
          }
        }
        for (size_t t = 0; t < v.numTimes(); ++t) {
          void* data = v.timeData(t);
          if (framework == "numpy") {
            nb::ndarray<nb::numpy> arr(data, shape, nb::handle(), {}, dtype);
            out.append(arr.cast(nb::rv_policy::reference_internal, self));
          } else if (framework == "torch" || framework == "pytorch") {
            nb::ndarray<nb::pytorch> arr(data, shape, nb::handle(), {}, dtype);
            out.append(arr.cast(nb::rv_policy::reference_internal, self));
          } else if (framework == "tensorflow" || framework == "tf") {
            nb::ndarray<nb::tensorflow> arr(data, shape, nb::handle(), {}, dtype);
            out.append(arr.cast(nb::rv_policy::reference_internal, self));
          } else if (framework == "jax") {
            nb::ndarray<nb::jax> arr(data, shape, nb::handle(), {}, dtype);
            out.append(arr.cast(nb::rv_policy::reference_internal, self));
          } else if (framework == "array_api") {
            nb::ndarray<nb::array_api> arr(data, shape, nb::handle(), {}, dtype);
            out.append(arr.cast(nb::rv_policy::reference_internal, self));
          } else if (framework == "memview") {
            nb::ndarray<nb::memview> arr(data, shape, nb::handle(), {}, dtype);
            out.append(arr.cast(nb::rv_policy::reference_internal, self));
          } else {
            throw ZException("Unknown framework: " + framework +
                             ". Use one of: numpy, torch, tensorflow, jax, array_api, memview.");
          }
        }
        return out;
      },
      "framework"_a = std::string("auto"),
      "Return image data as a list of arrays in the requested framework.\n\n"
      "Args:\n"
      "  framework (str): One of 'auto', 'numpy', 'torch', 'tensorflow', 'jax',\n"
      "    'array_api', or 'memview'. 'auto' mirrors the input framework when the\n"
      "    image was created zero-copy from arrays, otherwise returns NumPy.\n\n"
      "Returns:\n"
      "  list[ndarray]: CPU-backed arrays that reference ZImg buffers and keep\n"
      "  the parent ZImg alive (reference_internal).\n")
    .def("resize",
         &ZImg::resize,
         "desWidth"_a,
         "desHeight"_a,
         "desDepth"_a,
         "interpolant"_a = Interpolant::Cubic,
         "antialiasing"_a = true,
         "antialiasingForNearest"_a = false,
         "useMultithreading"_a = true,
         "Resize the image to the specified (width, height, depth) using the chosen interpolant.\n"
         "Antialiasing is applied where applicable; set 'useMultithreading' to control threading.")
    .def("zoom",
         &ZImg::zoom,
         "scaleX"_a,
         "scaleY"_a,
         "scaleZ"_a = 1.0,
         "interpolant"_a = Interpolant::Cubic,
         "antialiasing"_a = true,
         "antialiasingForNearest"_a = false,
         "Zoom the image by (scaleX, scaleY, scaleZ) using the specified interpolant.")
    .def("blockDownsample",
         &ZImg::blockDownsample,
         "blockWidth"_a,
         "blockHeight"_a,
         "blockDepth"_a,
         "mergeMode"_a,
         "Downsample the image using non-overlapping blocks and the given merge mode.")
    .def("secureDivideBy",
         &ZImg::secureDivideBy,
         "rhs"_a,
         "Safely divide this image by another image 'rhs' (elementwise), guarding against division pitfalls.")
  // MSVC (cl) emits spurious C4686 warnings for nanobind's operator
  // placeholders (nb::self ...), which are implemented via templated
  // helper UDTs in nanobind::detail. The warning complains about a
  // possible change in behavior due to UDT return calling convention,
  // but there is no actual ABI or behavioral issue here; it's a false
  // positive triggered by the placeholder expression templates.
  // Suppress it narrowly for this block on MSVC only.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4686)
#endif
    .def(nb::self + nb::self)
    .def(nb::self += nb::self)
    .def(nb::self - nb::self)
#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-assign-overloaded"
#endif
    .def(nb::self -= nb::self)
#ifdef __clang__
#pragma GCC diagnostic pop
#endif
    .def(nb::self * nb::self)
    .def(nb::self *= nb::self)
    .def(nb::self / nb::self)
#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-assign-overloaded"
#endif
    .def(nb::self /= nb::self)
#ifdef __clang__
#pragma GCC diagnostic pop
#endif
    .def(nb::self + double())
    .def(nb::self += double())
    .def(nb::self - double())
    .def(nb::self -= double())
    .def(nb::self * double())
    .def(nb::self *= double())
    .def(nb::self / double())
    .def(nb::self /= double())
    .def(nb::self + int32_t())
    .def(nb::self += int32_t())
    .def(nb::self - int32_t())
    .def(nb::self -= int32_t())
    .def(nb::self * int32_t())
    .def(nb::self *= int32_t())
    .def(nb::self / int32_t())
    .def(nb::self /= int32_t())
    .def(nb::self + uint32_t())
    .def(nb::self += uint32_t())
    .def(nb::self - uint32_t())
    .def(nb::self -= uint32_t())
    .def(nb::self * uint32_t())
    .def(nb::self *= uint32_t())
    .def(nb::self / uint32_t())
    .def(nb::self /= uint32_t())
    .def(nb::self + int64_t())
    .def(nb::self += int64_t())
    .def(nb::self - int64_t())
    .def(nb::self -= int64_t())
    .def(nb::self * int64_t())
    .def(nb::self *= int64_t())
    .def(nb::self / int64_t())
    .def(nb::self /= int64_t())
    .def(nb::self + uint64_t())
    .def(nb::self += uint64_t())
    .def(nb::self - uint64_t())
    .def(nb::self -= uint64_t())
    .def(nb::self * uint64_t())
    .def(nb::self *= uint64_t())
    .def(nb::self / uint64_t())
    .def(nb::self /= uint64_t())
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    .def("__repr__", [](const ZImg& v) {
      return fmt::format("<_imgpy.ZImg {}>", v.info().toString());
    });

  nb::class_<ZPunctum>(m, "ZPunctum")
    .def(nb::init<>())
    .def(nb::init<double, double, double, double>(), "x"_a, "y"_a, "z"_a, "r"_a)
    .def(nb::init<const Eigen::MatrixXi&, const Eigen::VectorXd&>(), "voxelLocations"_a, "voxelIntensities"_a)
    .def_rw("name", &ZPunctum::name)
    .def_rw("comment", &ZPunctum::comment)
    .def_prop_rw("x", &ZPunctum::x, &ZPunctum::setX)
    .def_prop_rw("y", &ZPunctum::y, &ZPunctum::setY)
    .def_prop_rw("z", &ZPunctum::z, &ZPunctum::setZ)
    .def_prop_rw("maxIntensity", &ZPunctum::maxIntensity, &ZPunctum::setMaxIntensity)
    .def_prop_rw("meanIntensity", &ZPunctum::meanIntensity, &ZPunctum::setMeanIntensity)
    .def_prop_rw("sDevOfIntensity", &ZPunctum::sDevOfIntensity, &ZPunctum::setSDevOfIntensity)
    .def_prop_rw("volSize", &ZPunctum::volSize, &ZPunctum::setVolSize)
    .def_prop_rw("mass", &ZPunctum::mass, &ZPunctum::setMass)
    .def_prop_rw("radius", &ZPunctum::radius, &ZPunctum::setRadius)
    .def_rw("property1", &ZPunctum::property1)
    .def_rw("property2", &ZPunctum::property2)
    .def_rw("property3", &ZPunctum::property3)
    .def_prop_rw("color", &ZPunctum::color, &ZPunctum::setColor)
    .def_prop_rw("score", &ZPunctum::score, &ZPunctum::setScore)
    .def_prop_rw("voxelLocations", &ZPunctum::voxelLocations, &ZPunctum::setVoxelLocations)
    .def_prop_rw("voxelIntensities", &ZPunctum::voxelIntensities, &ZPunctum::setVoxelIntensities)
    .def("updateFromVoxelsList", &ZPunctum::updateFromVoxelsList, "conf"_a = 0.95)
    .def("containsSignal", &ZPunctum::containsSignal)
    .def("mergeWith", &ZPunctum::mergeWith, "otherPunctum"_a, "conf"_a = 0.95)
    .def("split", &ZPunctum::split, "num"_a, "conf"_a = 0.95)
    .def_static(
      "merge",
      [](const std::list<ZPunctum>& punctumList, double conf) {
        return ZPunctum::merge(punctumList.begin(), punctumList.end(), conf);
      },
      "punctumList"_a,
      "conf"_a = 0.95)
    .def("__repr__", [](const ZPunctum& v) {
      return fmt::format("<_imgpy.ZPunctum {}>", v.toString());
    });

  nb::class_<ZPuncta>(m, "ZPuncta")
    .def(nb::init<>())
    .def(nb::init<const std::list<ZPunctum>&>())
    .def(nb::init<const QString&>(), "filename"_a)
    .def_rw("data", &ZPuncta::data)
    .def("save", &ZPuncta::save, "filename"_a, "format"_a = QString())
    .def("__repr__", [](const ZPuncta& v) {
      return fmt::format("<_imgpy.ZPuncta {}>", v.toString());
    });

  nb::class_<ZStitchImage>(m, "ZStitchImage")
    .def(nb::init<>())
    .def("setInputFilenames", &ZStitchImage::setInputFilenames, "filenames"_a, "scene"_a = 0)
    .def("setResultFilename", &ZStitchImage::setResultFilename, "filename"_a)
    .def("setUseChannels", &ZStitchImage::setUseChannels, "channels"_a = std::vector<size_t>())
    .def("setRemoveBackgroundForChannels",
         &ZStitchImage::setRemoveBackgroundForChannels,
         "channels"_a = std::vector<size_t>())
    .def("setDownsampleBeforeStitching",
         &ZStitchImage::setDownsampleBeforeStitching,
         "blockWidth"_a,
         "blockHeight"_a,
         "blockDepth"_a,
         "blockMergeMode"_a = ImgMergeMode::Mean)
    .def("setStartResolution", &ZStitchImage::setStartResolution, "intvX"_a, "intvY"_a, "intvZ"_a)
    .def("setConcatenateOnly", &ZStitchImage::setConcatenateOnly)
    .def("setMergeMode", &ZStitchImage::setMergeMode, "mode"_a)
    .def("setMaxOverlapRate", &ZStitchImage::setMaxOverlapRate, "maxOverlapRate"_a)
    .def("setTileGrid", &ZStitchImage::setTileGrid, "tileGrid"_a)
    .def("setTileGridFromMatrixFile", &ZStitchImage::setTileGridFromMatrixFile, "filename"_a)
    .def("setTileGridFromTileSelectionImage", &ZStitchImage::setTileGridFromTileSelectionImage, "filename"_a)
    .def("setConnInfoFromConnTextFile", &ZStitchImage::setConnInfoFromConnTextFile, "filename"_a)
    .def("setTileGridFromLayout", &ZStitchImage::setTileGridFromLayout, "numRows"_a, "numCols"_a)
    .def("setRestitch", &ZStitchImage::setRestitch)
    .def("setBlindStitching", &ZStitchImage::setBlindStitching)
    .def("set2ndInput",
         &ZStitchImage::set2ndInput,
         "fns"_a,
         "scene"_a,
         "chsToUse"_a,
         "chsToRemoveBackground"_a,
         "commonChannelOfInput"_a,
         "commonChannelOf2ndInput"_a)
    .def("setUseMultithreading", &ZStitchImage::setUseMultithreading, "v"_a)
    .def("setLogFile", &ZStitchImage::setLogFile, "logfilename"_a)
    .def("loadTask", &ZStitchImage::loadTask, "filename"_a)
    .def("saveTask", &ZStitchImage::saveTask, "filename"_a)
    .def("run", &ZStitchImage::run)
    .def("__repr__", [](const ZStitchImage& v) {
      return fmt::format("<_imgpy.ZStitchImage {}>", v.toString());
    });

  nb::class_<ZPunctaDetection>(m, "ZPunctaDetection")
    .def(nb::init<>())
    .def(nb::init<const QString&, size_t, size_t, size_t>(),
         "filename"_a,
         "punctaChannel"_a = 0,
         "t"_a = 0,
         "scene"_a = 0)
    .def(nb::init<const QString&, const ZImgInfo&, size_t, size_t, size_t>(),
         "filename"_a,
         "imgInfo"_a,
         "punctaChannel"_a = 0,
         "t"_a = 0,
         "scene"_a = 0)
    .def("setInputFile",
         &ZPunctaDetection::setInputFile,
         "filename"_a,
         "punctaChannel"_a = 0,
         "t"_a = 0,
         "scene"_a = 0,
         "voxelSizeInUmX"_a = -1.0,
         "voxelSizeInUmY"_a = -1.0,
         "voxelSizeInUmZ"_a = -1.0)
    .def("setResultPunctaFilename", &ZPunctaDetection::setResultPunctaFilename, "filename"_a)
    .def("setResultSomaPunctaFilename", &ZPunctaDetection::setResultSomaPunctaFilename, "filename"_a)
    .def("setPunctaThreshold", &ZPunctaDetection::setPunctaThreshold, "thre"_a)
    .def("setSomaPunctaThreshold", &ZPunctaDetection::setSomaPunctaThreshold, "thre"_a)
    .def("setSplitThreshold", &ZPunctaDetection::setSplitThreshold, "thre"_a)
    .def("setConfidenceRegionForRadiusEstimate",
         &ZPunctaDetection::setConfidenceRegionForRadiusEstimate,
         "confRadius"_a)
    .def("setConfidenceRegionForOverlapArea", &ZPunctaDetection::setConfidenceRegionForOverlapArea, "confOverlapArea"_a)
    .def("setOverlapRateThreshold", &ZPunctaDetection::setOverlapRateThreshold, "thre"_a)
    .def("setSeedSizeThreshold", &ZPunctaDetection::setSeedSizeThreshold, "thre"_a)
    .def("setUseMultithreading", &ZPunctaDetection::setUseMultithreading, "v"_a)
    .def("setDendriteChannel", &ZPunctaDetection::setDendriteChannel, "dendriteChannel"_a)
    .def("setMaxDendriteTubeRadiusInUm", &ZPunctaDetection::setMaxDendriteTubeRadiusInUm, "maxDendriteTubeRadius"_a)
    .def("setDendriteThreshold", &ZPunctaDetection::setDendriteThreshold, "thre"_a)
    .def("setSwcFiles", &ZPunctaDetection::setSwcFiles, "swcFiles"_a)
    .def("setMaxDistToBranchInUm", &ZPunctaDetection::setMaxDistToBranchInUm, "dist"_a)
    .def("setAmbiguousFactor", &ZPunctaDetection::setAmbiguousFactor, "factor"_a)
    .def("setLogFile", &ZPunctaDetection::setLogFile, "logfilename"_a)
    .def("loadTask", &ZPunctaDetection::loadTask, "filename"_a)
    .def("saveTask", &ZPunctaDetection::saveTask, "filename"_a)
    .def("run", &ZPunctaDetection::run)
    .def("__repr__", [](const ZPunctaDetection& v) {
      return fmt::format("<_imgpy.ZPunctaDetection {}>", v.toString());
    });

  nb::class_<TraceConfig>(m, "TraceConfig")
    .def(nb::init<>())
    .def_rw("minAutoScore", &TraceConfig::minAutoScore)
    .def_rw("minManualScore", &TraceConfig::minManualScore)
    .def_rw("minSeedScore", &TraceConfig::minSeedScore)
    .def_rw("min2dScore", &TraceConfig::min2dScore)
    .def_rw("refit", &TraceConfig::refit)
    .def_rw("spTest", &TraceConfig::spTest)
    .def_rw("crossoverTest", &TraceConfig::crossoverTest)
    .def_rw("tuneEnd", &TraceConfig::tuneEnd)
    .def_rw("edgePath", &TraceConfig::edgePath)
    .def_rw("enhanceMask", &TraceConfig::enhanceMask)
    .def_rw("seedMethod", &TraceConfig::seedMethod)
    .def_rw("recover", &TraceConfig::recover)
    .def_rw("chainScreenCount", &TraceConfig::chainScreenCount)
    .def_rw("maxEucDist", &TraceConfig::maxEucDist)
    .def("__repr__", [](const TraceConfig& cfg) {
      return fmt::format("<_imgpy.TraceConfig minAutoScore={} minSeedScore={} seedMethod={} recover={}>",
                         cfg.minAutoScore,
                         cfg.minSeedScore,
                         cfg.seedMethod,
                         cfg.recover);
    });

  nb::class_<ZNeutubeSkeletonizeProcess>(m, "ZNeutubeSkeletonize")
    .def(nb::init<>())
    .def("setInputImageSource", &ZNeutubeSkeletonizeProcess::setInputImageSource, "imgSource"_a)
    .def("setInputImagePath", &ZNeutubeSkeletonizeProcess::setInputImagePath, "filename"_a)
    .def("setInputFile", &ZNeutubeSkeletonizeProcess::setInputImagePath, "filename"_a)
    .def("setSkeletonizeConfigPath", &ZNeutubeSkeletonizeProcess::setSkeletonizeConfigPath, "filename"_a)
    .def(
      "setSkeletonizeConfig",
      [](ZNeutubeSkeletonizeProcess& self, nb::handle cfg) {
        self.setSkeletonizeConfig(pythonObjectToJsonObject(cfg, "cfg"));
      },
      "cfg"_a)
    .def(
      "setDownsampleIntervalOverride",
      [](ZNeutubeSkeletonizeProcess& self, nb::handle value) {
        if (value.is_none()) {
          self.setDownsampleIntervalOverride(std::nullopt);
          return;
        }
        self.setDownsampleIntervalOverride(
          vectorToArray3<int>(nb::cast<std::vector<int>>(value), "downsampleIntervalOverride"));
      },
      "downsampleIntervalOverride"_a)
    .def("setVerbose", &ZNeutubeSkeletonizeProcess::setVerbose, "v"_a)
    .def("setOutputSwcPath", &ZNeutubeSkeletonizeProcess::setOutputSwcPath, "filename"_a)
    .def("outputSwcPath", &ZNeutubeSkeletonizeProcess::outputSwcPath)
    .def("hasResult", &ZNeutubeSkeletonizeProcess::hasResult)
    .def("setLogFile", &ZNeutubeSkeletonizeProcess::setLogFile, "logfilename"_a)
    .def("loadTask", &ZNeutubeSkeletonizeProcess::loadTask, "filename"_a)
    .def("saveTask", &ZNeutubeSkeletonizeProcess::saveTask, "filename"_a)
    .def("run", &ZNeutubeSkeletonizeProcess::run)
    .def("__repr__", [](const ZNeutubeSkeletonizeProcess& v) {
      return fmt::format("<_imgpy.ZNeutubeSkeletonize {}>", v.toString());
    });

  nb::class_<ZNeutubeAutoTraceProcess>(m, "ZNeutubeAutoTrace")
    .def(nb::init<>())
    .def("setInputImageSource", &ZNeutubeAutoTraceProcess::setInputImageSource, "imgSource"_a)
    .def("setInputImagePath", &ZNeutubeAutoTraceProcess::setInputImagePath, "filename"_a)
    .def("setInputFile", &ZNeutubeAutoTraceProcess::setInputImagePath, "filename"_a)
    .def("setSelectedChannelTime", &ZNeutubeAutoTraceProcess::setSelectedChannelTime, "channel"_a, "time"_a)
    .def("setZToXYRatio", &ZNeutubeAutoTraceProcess::setZToXYRatio, "zToXYRatio"_a)
    .def("setTraceConfigPath", &ZNeutubeAutoTraceProcess::setTraceConfigPath, "filename"_a)
    .def(
      "setTraceConfig",
      [](ZNeutubeAutoTraceProcess& self, nb::handle cfg) {
        self.setTraceConfig(pythonObjectToJsonObject(cfg, "cfg"));
      },
      "cfg"_a)
    .def("clearTraceConfig", &ZNeutubeAutoTraceProcess::clearTraceConfig)
    .def("setTraceLevel", &ZNeutubeAutoTraceProcess::setTraceLevel, "level"_a)
    .def("setAlgoConfigOverrides", &ZNeutubeAutoTraceProcess::setAlgoConfigOverrides, "cfg"_a)
    .def("clearAlgoConfigOverrides", &ZNeutubeAutoTraceProcess::clearAlgoConfigOverrides)
    .def("setDoResampleAfterTracing", &ZNeutubeAutoTraceProcess::setDoResampleAfterTracing, "enabled"_a)
    .def(
      "setSignalDownsampleRatio",
      [](ZNeutubeAutoTraceProcess& self, const std::vector<size_t>& ratio) {
        self.setSignalDownsampleRatio(vectorToArray3<size_t>(ratio, "signalDownsampleRatio"));
      },
      "ratio"_a)
    .def("setDocHasAnySwc", &ZNeutubeAutoTraceProcess::setDocHasAnySwc, "v"_a)
    .def("setOutputSwcPath", &ZNeutubeAutoTraceProcess::setOutputSwcPath, "filename"_a)
    .def("outputSwcPath", &ZNeutubeAutoTraceProcess::outputSwcPath)
    .def("hasResult", &ZNeutubeAutoTraceProcess::hasResult)
    .def("setLogFile", &ZNeutubeAutoTraceProcess::setLogFile, "logfilename"_a)
    .def("loadTask", &ZNeutubeAutoTraceProcess::loadTask, "filename"_a)
    .def("saveTask", &ZNeutubeAutoTraceProcess::saveTask, "filename"_a)
    .def("run", &ZNeutubeAutoTraceProcess::run)
    .def("__repr__", [](const ZNeutubeAutoTraceProcess& v) {
      return fmt::format("<_imgpy.ZNeutubeAutoTrace {}>", v.toString());
    });

  nb::class_<ZNeutubeBlockedAutoTraceProcess>(m, "ZNeutubeBlockedAutoTrace")
    .def(nb::init<>())
    .def("setInputImageSource", &ZNeutubeBlockedAutoTraceProcess::setInputImageSource, "imgSource"_a)
    .def("setInputImagePath", &ZNeutubeBlockedAutoTraceProcess::setInputImagePath, "filename"_a)
    .def("setInputFile", &ZNeutubeBlockedAutoTraceProcess::setInputImagePath, "filename"_a)
    .def("setSignalInfo", &ZNeutubeBlockedAutoTraceProcess::setSignalInfo, "imgInfo"_a)
    .def("setDatasetId", &ZNeutubeBlockedAutoTraceProcess::setDatasetId, "datasetId"_a)
    .def("setSelectedChannelTime", &ZNeutubeBlockedAutoTraceProcess::setSelectedChannelTime, "channel"_a, "time"_a)
    .def("setZToXYRatio", &ZNeutubeBlockedAutoTraceProcess::setZToXYRatio, "zToXYRatio"_a)
    .def(
      "setSignalDownsampleRatio",
      [](ZNeutubeBlockedAutoTraceProcess& self, const std::vector<size_t>& ratio) {
        self.setSignalDownsampleRatio(vectorToArray3<size_t>(ratio, "signalDownsampleRatio"));
      },
      "ratio"_a)
    .def("setBlockCoreSize", &ZNeutubeBlockedAutoTraceProcess::setBlockCoreSize, "voxels"_a)
    .def("setBlockCoreSizeXYZ", &ZNeutubeBlockedAutoTraceProcess::setBlockCoreSizeXYZ, "coreX"_a, "coreY"_a, "coreZ"_a)
    .def("setBlockHalo", &ZNeutubeBlockedAutoTraceProcess::setBlockHalo, "voxels"_a)
    .def("setTraceConfigPath", &ZNeutubeBlockedAutoTraceProcess::setTraceConfigPath, "filename"_a)
    .def(
      "setTraceConfig",
      [](ZNeutubeBlockedAutoTraceProcess& self, nb::handle cfg) {
        self.setTraceConfig(pythonObjectToJsonObject(cfg, "cfg"));
      },
      "cfg"_a)
    .def("clearTraceConfig", &ZNeutubeBlockedAutoTraceProcess::clearTraceConfig)
    .def("setTraceLevel", &ZNeutubeBlockedAutoTraceProcess::setTraceLevel, "level"_a)
    .def("setAlgoConfigOverrides", &ZNeutubeBlockedAutoTraceProcess::setAlgoConfigOverrides, "cfg"_a)
    .def("clearAlgoConfigOverrides", &ZNeutubeBlockedAutoTraceProcess::clearAlgoConfigOverrides)
    .def("setDoResampleAfterTracing", &ZNeutubeBlockedAutoTraceProcess::setDoResampleAfterTracing, "enabled"_a)
    .def("setDocHasAnySwc", &ZNeutubeBlockedAutoTraceProcess::setDocHasAnySwc, "v"_a)
    .def("setOutputSwcPath", &ZNeutubeBlockedAutoTraceProcess::setOutputSwcPath, "filename"_a)
    .def("setOutputSessionDir", &ZNeutubeBlockedAutoTraceProcess::setOutputSessionDir, "dirname"_a)
    .def("outputSwcPath", &ZNeutubeBlockedAutoTraceProcess::outputSwcPath)
    .def("outputSessionDir", &ZNeutubeBlockedAutoTraceProcess::outputSessionDir)
    .def("hasResult", &ZNeutubeBlockedAutoTraceProcess::hasResult)
    .def("setLogFile", &ZNeutubeBlockedAutoTraceProcess::setLogFile, "logfilename"_a)
    .def("loadTask", &ZNeutubeBlockedAutoTraceProcess::loadTask, "filename"_a)
    .def("saveTask", &ZNeutubeBlockedAutoTraceProcess::saveTask, "filename"_a)
    .def("run", &ZNeutubeBlockedAutoTraceProcess::run)
    .def("__repr__", [](const ZNeutubeBlockedAutoTraceProcess& v) {
      return fmt::format("<_imgpy.ZNeutubeBlockedAutoTrace {}>", v.toString());
    });

  nb::class_<ZSwcSubtract>(m, "ZSwcSubtract")
    .def(nb::init<>())
    .def("setInputSwcFilename", &ZSwcSubtract::setInputSwcFilename, "filename"_a)
    .def("setSubtractSwcFilenames", &ZSwcSubtract::setSubtractSwcFilenames, "filenames"_a)
    .def("setOutputSwcFilename", &ZSwcSubtract::setOutputSwcFilename, "filename"_a)
    .def("setLogFile", &ZSwcSubtract::setLogFile, "logfilename"_a)
    .def("loadTask", &ZSwcSubtract::loadTask, "filename"_a)
    .def("saveTask", &ZSwcSubtract::saveTask, "filename"_a)
    .def("run", &ZSwcSubtract::run)
    .def("__repr__", [](const ZSwcSubtract& v) {
      return fmt::format("<_imgpy.ZSwcSubtract {}>", v.toString());
    });

  nb::class_<ZSectionsRegistration>(m, "ZSectionsRegistration")
    .def(nb::init<>())
    .def("setInputOutput", &ZSectionsRegistration::setInputOutput, "inputFiles"_a, "resultFile"_a, "fixedSliceIndex"_a)
    .def("setReferenceChannel", &ZSectionsRegistration::setReferenceChannel, "refChannel"_a)
    .def("setRemoveBackground", &ZSectionsRegistration::setRemoveBackground, "v"_a)
    .def("setRemoveHighForeground", &ZSectionsRegistration::setRemoveHighForeground, "v"_a)
    .def("setAllowFlip", &ZSectionsRegistration::setAllowFlip, "v"_a)
    .def("setBrightBackground", &ZSectionsRegistration::setBrightBackground, "v"_a)
    .def("setMetric", &ZSectionsRegistration::setMetric, "metric"_a)
    .def("setTransform", &ZSectionsRegistration::setTransform, "transform"_a)
    .def("setOptimizer", &ZSectionsRegistration::setOptimizer, "optimizer"_a)
    .def("setUseMultithreading", &ZSectionsRegistration::setUseMultithreading, "v"_a)
    .def("setNumScales", &ZSectionsRegistration::setNumScales, "numScales"_a)
    .def("setNumNeighbors", &ZSectionsRegistration::setNumNeighbors, "numNeighbors"_a)
    .def("setLogFile", &ZSectionsRegistration::setLogFile, "logfilename"_a)
    .def("loadTask", &ZSectionsRegistration::loadTask, "filename"_a)
    .def("saveTask", &ZSectionsRegistration::saveTask, "filename"_a)
    .def("run", &ZSectionsRegistration::run)
    .def("__repr__", [](const ZSectionsRegistration& v) {
      return fmt::format("<_imgpy.ZSectionsRegistration {}>", v.toString());
    });

  nb::class_<ZChromaticShiftCorrection>(m, "ZChromaticShiftCorrection")
    .def(nb::init<>())
    .def("setInputOutput", &ZChromaticShiftCorrection::setInputOutput, "inputFile"_a, "resultFile"_a)
    .def("setReferenceChannel", &ZChromaticShiftCorrection::setReferenceChannel, "refChannel"_a)
    .def("setTargetChannel", &ZChromaticShiftCorrection::setTargetChannel, "targetChannel"_a)
    .def("setRemoveBackground", &ZChromaticShiftCorrection::setRemoveBackground, "v"_a)
    .def("setRemoveHighForeground", &ZChromaticShiftCorrection::setRemoveHighForeground, "v"_a)
    .def("setBrightBackground", &ZChromaticShiftCorrection::setBrightBackground, "v"_a)
    .def("setMethod", &ZChromaticShiftCorrection::setMethod, "method"_a)
    .def("setMetric", &ZChromaticShiftCorrection::setMetric, "metric"_a)
    .def("setTransform", &ZChromaticShiftCorrection::setTransform, "transform"_a)
    .def("setOptimizer", &ZChromaticShiftCorrection::setOptimizer, "optimizer"_a)
    .def("setUseMultithreading", &ZChromaticShiftCorrection::setUseMultithreading, "v"_a)
    .def("setNumScales", &ZChromaticShiftCorrection::setNumScales, "numScales"_a)
    .def("setLogFile", &ZChromaticShiftCorrection::setLogFile, "logfilename"_a)
    .def("loadTask", &ZChromaticShiftCorrection::loadTask, "filename"_a)
    .def("saveTask", &ZChromaticShiftCorrection::saveTask, "filename"_a)
    .def("run", &ZChromaticShiftCorrection::run)
    .def("__repr__", [](const ZChromaticShiftCorrection& v) {
      return fmt::format("<_imgpy.ZChromaticShiftCorrection {}>", v.toString());
    });

  nb::class_<ZImgNCCMatch>(m, "ZImgNCCMatch")
    .def(nb::init<const ZImg&, const ZImg&, size_t, size_t>(),
         "fixedImg"_a,
         "movingImg"_a,
         "fixedT"_a = 0,
         "movingT"_a = 0)
    .def("useFixedImgChannels", &ZImgNCCMatch::useFixedImgChannels, "chs"_a)
    .def("useMovingImgChannels", &ZImgNCCMatch::useMovingImgChannels, "chs"_a)
    .def("useAllFixedImgChannels", &ZImgNCCMatch::useAllFixedImgChannels)
    .def("useAllMovingImgChannels", &ZImgNCCMatch::useAllMovingImgChannels)
    .def("removeBackgroundForFixedImgChannels", &ZImgNCCMatch::removeBackgroundForFixedImgChannels, "chs"_a)
    .def("removeBackgroundForMovingImgChannels", &ZImgNCCMatch::removeBackgroundForMovingImgChannels, "chs"_a)
    .def("disableRemoveBackgroundForAllFixedImgChannels", &ZImgNCCMatch::disableRemoveBackgroundForAllFixedImgChannels)
    .def("disableRemoveBackgroundForAllMovingImgChannels",
         &ZImgNCCMatch::disableRemoveBackgroundForAllMovingImgChannels)
    .def("computeNCCOfOffset", &ZImgNCCMatch::computeNCCOfOffset, "offset"_a)
    .def("computeNCC", &ZImgNCCMatch::computeNCC)
    .def("computeMovingImgOffset", &ZImgNCCMatch::computeMovingImgOffset_Python)
    .def("computeMovingImgOffsetMR", &ZImgNCCMatch::computeMovingImgOffsetMR_Python, "intvX"_a, "intvY"_a, "intvZ"_a)
    .def("refineMovingImgOffset",
         &ZImgNCCMatch::refineMovingImgOffset_Python,
         "offset"_a,
         "radiusX"_a,
         "radiusY"_a,
         "radiusZ"_a)
    .def("refineMovingImgOffsetMR",
         &ZImgNCCMatch::refineMovingImgOffsetMR_Python,
         "offset"_a,
         "radiusX"_a,
         "radiusY"_a,
         "radiusZ"_a,
         "intvX"_a,
         "intvY"_a,
         "intvZ"_a)
    .def("__repr__", [](const ZImgNCCMatch&) {
      return fmt::format("<_imgpy.ZImgNCCMatch>");
    });

  struct ZROIUtilsPy
  {};

  nb::class_<ZROIUtilsPy>(m, "ZROIUtils")
    .def_static(
      "splineToMask",
      [](const nb::ndarray<nb::numpy, const double>& spline) -> std::tuple<ZImg, index_t, index_t> {
        ZROIMaskOperation2D op;
        op.isAdd = true;
        op.type = ZROIMaskShapeType::Spline;
        op.poly = roiPointsFromArray(spline, "spline");
        validateROIPoints(op.poly, op.type, "Spline");
        std::vector<ZROIMaskOperation2D> ops;
        ops.push_back(std::move(op));
        return ZROIMaskRasterizer::shapeToMask(ops);
      },
      "spline"_a.noconvert())
    .def_static(
      "rectToMask",
      [](const nb::ndarray<nb::numpy, const double>& rect) -> std::tuple<ZImg, index_t, index_t> {
        ZROIMaskOperation2D op;
        op.isAdd = true;
        op.type = ZROIMaskShapeType::Rect;
        op.poly = roiPointsFromArray(rect, "rect");
        validateROIPoints(op.poly, op.type, "Rect");
        std::vector<ZROIMaskOperation2D> ops;
        ops.push_back(std::move(op));
        return ZROIMaskRasterizer::shapeToMask(ops);
      },
      "rect"_a.noconvert())
    .def_static(
      "ellipseToMask",
      [](const nb::ndarray<nb::numpy, const double>& ellipse) -> std::tuple<ZImg, index_t, index_t> {
        ZROIMaskOperation2D op;
        op.isAdd = true;
        op.type = ZROIMaskShapeType::Ellipse;
        op.poly = roiPointsFromArray(ellipse, "ellipse");
        validateROIPoints(op.poly, op.type, "Ellipse");
        std::vector<ZROIMaskOperation2D> ops;
        ops.push_back(std::move(op));
        return ZROIMaskRasterizer::shapeToMask(ops);
      },
      "ellipse"_a.noconvert())
    .def_static(
      "polygonToMask",
      [](const nb::ndarray<nb::numpy, const double>& polygon) -> std::tuple<ZImg, index_t, index_t> {
        ZROIMaskOperation2D op;
        op.isAdd = true;
        op.type = ZROIMaskShapeType::Polygon;
        op.poly = roiPointsFromArray(polygon, "polygon");
        validateROIPoints(op.poly, op.type, "Polygon");
        std::vector<ZROIMaskOperation2D> ops;
        ops.push_back(std::move(op));
        return ZROIMaskRasterizer::shapeToMask(ops);
      },
      "polygon"_a.noconvert())
    .def_static(
      "shapeToMask",
      [](nb::handle shapeOps) -> std::tuple<ZImg, index_t, index_t> {
        std::vector<ZROIMaskOperation2D> ops;
        for (nb::handle item : nb::borrow<nb::iterable>(shapeOps)) {
          const nb::tuple t = nb::cast<nb::tuple>(item);
          if (t.size() != 3) {
            throw nb::type_error("shapeToMask expects entries of the form (points, type, isAdd)");
          }
          const auto points = nb::cast<nb::ndarray<nb::numpy, const double>>(t[0]);
          const std::string type = nb::cast<std::string>(t[1]);
          const bool isAdd = nb::cast<bool>(t[2]);

          ZROIMaskOperation2D op;
          op.isAdd = isAdd;
          op.type = roiShapeTypeFromString(type);
          op.poly = roiPointsFromArray(points, "points");
          const std::string ctx = "shape '" + type + "'";
          validateROIPoints(op.poly, op.type, ctx);
          ops.push_back(std::move(op));
        }
        return ZROIMaskRasterizer::shapeToMask(ops);
      },
      "shapes"_a)
    .def("__repr__", [](const ZROIUtilsPy&) {
      return fmt::format("<_imgpy.ZROIUtils>");
    });

  nb::class_<ZImgSubBlock, PyZImgSubBlock<>>(m, "ZImgSubBlock")
    .def(nb::init<index_t, index_t, index_t, index_t, size_t, size_t, size_t, size_t, size_t, size_t>(),
         "t"_a,
         "x"_a,
         "y"_a,
         "z"_a,
         "width"_a,
         "height"_a,
         "depth"_a,
         "xRatio"_a,
         "yRatio"_a,
         "zRatio"_a)
    .def("read", &ZImgSubBlock::read)
    .def("readInfo", &ZImgSubBlock::readInfo)
    .def("__repr__", [](const ZImgSubBlock&) {
      return fmt::format("<_imgpy.ZImgSubBlock>");
    });
  nb::class_<ZImgTileSubBlock, ZImgSubBlock, PyZImgTileSubBlock<>>(m, "ZImgTileSubBlock")
    .def(nb::init<const ZImgSource&, size_t, size_t, size_t, ImgMergeMode>(),
         "source"_a,
         "xRatio"_a = 1,
         "yRatio"_a = 1,
         "zRatio"_a = 1,
         "downsampleCombineMode"_a = ImgMergeMode::Interpolation)
    .def("__repr__", [](const ZImgTileSubBlock&) {
      return fmt::format("<_imgpy.ZImgTileSubBlock>");
    });

  nb::class_<ZImgSliceProvider, PyZImgSliceProvider>(m, "ZImgSliceProvider")
    .def(nb::init<>())
    .def("imgInfo", &ZImgSliceProvider::imgInfo)
    .def("slice", &ZImgSliceProvider::slice, "z"_a, "t"_a)
    .def("allSlices", &ZImgSliceProvider::allSlices, "t"_a)
    .def("wholeImg", &ZImgSliceProvider::wholeImg)
    .def("__repr__", [](const ZImgSliceProvider&) {
      return fmt::format("<_imgpy.ZImgSliceProvider>");
    });

  nb::class_<ZImgBlockProvider, PyZImgBlockProvider>(m, "ZImgBlockProvider")
    .def(nb::init<>())
    .def("imgInfo", &ZImgBlockProvider::imgInfo)
    .def("numBlocks", &ZImgBlockProvider::numBlocks)
    .def("block", &ZImgBlockProvider::block, "blockIdx"_a)
    .def("blockCoord", &ZImgBlockProvider::blockCoord, "blockIdx"_a)
    .def("wholeImg", &ZImgBlockProvider::wholeImg)
    .def("__repr__", [](const ZImgBlockProvider&) {
      return fmt::format("<_imgpy.ZImgBlockProvider>");
    });

  nb::class_<ZImgMerge>(m, "ZImgMerge")
    .def(nb::init<>())
    .def("addImg", &ZImgMerge::addImg, "img"_a, "loc"_a, "imgName"_a = QString(""))
    .def("addImgPair",
         &ZImgMerge::addImgPair,
         "img1"_a,
         "img2"_a,
         "img2Offset"_a,
         "connectionCost"_a = 0,
         "img1Name"_a = QString(""),
         "img2Name"_a = QString(""))
    .def("resolveLocations", &ZImgMerge::resolveLocations)
    .def("setMergeMode", &ZImgMerge::setMergeMode, "mode"_a = ImgMergeMode::Max)
    .def("save", &ZImgMerge::save, "filename"_a, "format"_a = FileFormat::Unknown, "paras"_a = ZImgWriteParameters())
    .def("__repr__", [](const ZImgMerge&) {
      return fmt::format("<_imgpy.ZImgMerge>");
    });

  nb::class_<ZImgAutoThreshold>(m, "ZImgAutoThreshold")
    .def(nb::init<>())
    .def("u8TriangleThre",
         &ZImgAutoThreshold::u8TriangleThre,
         "filename"_a,
         "minValue"_a,
         "maxValue"_a,
         "c"_a = 0,
         "t"_a = 0,
         "scene"_a = 0,
         "mask"_a = std::vector<ZVoxelCoordinate>())
    .def("__repr__", [](const ZImgAutoThreshold&) {
      return fmt::format("<_imgpy.ZImgAutoThreshold>");
    });

  nb::class_<SwcNode>(m, "SwcNode")
    .def(nb::init<int64_t, int64_t, double, double, double, double, int64_t>(),
         "id"_a = -1,
         "type"_a = -1,
         "x"_a = 0.,
         "y"_a = 0.,
         "z"_a = 0.,
         "radius"_a = -1.,
         "parentID"_a = -2)
    .def_rw("id", &SwcNode::id)
    .def_rw("type", &SwcNode::type)
    .def_rw("x", &SwcNode::x)
    .def_rw("y", &SwcNode::y)
    .def_rw("z", &SwcNode::z)
    .def_rw("radius", &SwcNode::radius)
    .def_rw("parentID", &SwcNode::parentID)
    .def_rw("label", &SwcNode::label)
    .def("__repr__", [](const SwcNode& v) {
      return fmt::format("<_imgpy.SwcNode {}>", v.toString());
    });

  nb::class_<ZSwc>(m, "ZSwc")
    .def(nb::init<>())
    .def(nb::init<const QString&>(), "filename"_a)
    .def("load", &ZSwc::load, "filename"_a)
    .def("save", &ZSwc::save, "filename"_a)
    .def("labelSomaAndOthers", &ZSwc::labelSomaAndOthers, "radiusThre"_a = 0., "somaType"_a = 1, "otherType"_a = 2)
    .def("resortPyramidal", &ZSwc::resortPyramidal, "basalType"_a = 3, "apicalType"_a = 4, "somaType"_a = 1)
    .def("resortID", &ZSwc::resortID)
    .def("__repr__", [](const ZSwc& v) {
      return fmt::format("<_imgpy.ZSwc {}>", v.toString());
    });

  nb::enum_<ZMesh::Type>(m, "ZMeshType")
    .value("TRIANGLES", ZMesh::Type::TRIANGLES)
    .value("TRIANGLE_STRIP", ZMesh::Type::TRIANGLE_STRIP)
    .value("TRIANGLE_FAN", ZMesh::Type::TRIANGLE_FAN);

  nb::class_<ZMesh>(m, "ZMesh")
    .def(nb::init<ZMesh::Type>(), "type"_a = ZMesh::Type::TRIANGLES)
    .def(nb::init<const QString&>(), "filename"_a)
    .def("load", nb::overload_cast<const QString&>(&ZMesh::load), "filename"_a)
    .def("save",
         nb::overload_cast<const QString&, const std::string&>(&ZMesh::save, nb::const_),
         "filename"_a,
         "format"_a = std::string())
    .def("toLabelImg",
         &ZMesh::toLabelImg,
         "width"_a = 0,
         "height"_a = 0,
         "depth"_a = 0,
         "tfmat"_a = glm::mat4(1.f),
         "tolerance"_a = 1e-6)
    .def_prop_rw("type", &ZMesh::type, &ZMesh::setType)
    .def_static(
      "createPunctaMesh",
      [](const ZPuncta& puncta, int resolution, const glm::mat4& tfmat) {
        ZMesh res;
        ZMesh::createPunctaMesh(puncta, res, resolution, tfmat);
        return res;
      },
      "puncta"_a,
      "resolution"_a = 32,
      "tfmat"_a = glm::mat4(1.f))
    .def_static(
      "createSwcMesh",
      [](const ZSwc& swc, int somaType, const glm::mat4& tfmat) {
        ZMesh rootMesh, somaMesh, neuriteMesh;
        ZMesh::createSwcMesh(swc, somaType, rootMesh, somaMesh, neuriteMesh, tfmat);
        return std::make_tuple(rootMesh, somaMesh, neuriteMesh);
      },
      "swc"_a,
      "somaType"_a = 1,
      "tfmat"_a = glm::mat4(1.f))
    .def_prop_rw(
      "vertices",
      [](const ZMesh& v) {
        return vecVecToArray(v.vertices());
      },
      [](ZMesh& v, const nb::ndarray<float>& array) {
        if (!zpy::isCPU(array)) {
          throw ZException("vertices ndarray must be CPU-backed");
        }
        v.setVertices(arrayToVecVec<3>(array));
      })
    .def_prop_rw(
      "normals",
      [](const ZMesh& v) {
        return vecVecToArray(v.normals());
      },
      [](ZMesh& v, const nb::ndarray<float>& array) {
        if (!zpy::isCPU(array)) {
          throw ZException("normals ndarray must be CPU-backed");
        }
        v.setNormals(arrayToVecVec<3>(array));
      })
    .def_prop_rw(
      "colors",
      [](const ZMesh& v) {
        return vecVecToArray(v.colors());
      },
      [](ZMesh& v, const nb::ndarray<float>& array) {
        if (!zpy::isCPU(array)) {
          throw ZException("colors ndarray must be CPU-backed");
        }
        v.setColors(arrayToVecVec<4>(array));
      })
    .def_prop_rw(
      "indices",
      [](const ZMesh& v) {
        return vectorToArray(v.indices());
      },
      [](ZMesh& v, const nb::ndarray<uint32_t>& array) {
        if (!zpy::isCPU(array)) {
          throw ZException("indices ndarray must be CPU-backed");
        }
        v.setIndices(arrayToVector(array));
      })
    .def_prop_rw(
      "textureCoordinates1D",
      [](const ZMesh& v) {
        return vectorToArray(v.textureCoordinates1D());
      },
      [](ZMesh& v, const nb::ndarray<float>& array) {
        if (!zpy::isCPU(array)) {
          throw ZException("textureCoordinates1D ndarray must be CPU-backed");
        }
        v.setTextureCoordinates(arrayToVector(array));
      })
    .def_prop_rw(
      "textureCoordinates2D",
      [](const ZMesh& v) {
        return vecVecToArray(v.textureCoordinates2D());
      },
      [](ZMesh& v, const nb::ndarray<float>& array) {
        if (!zpy::isCPU(array)) {
          throw ZException("textureCoordinates2D ndarray must be CPU-backed");
        }
        v.setTextureCoordinates(arrayToVecVec<2>(array));
      })
    .def_prop_rw(
      "textureCoordinates3D",
      [](const ZMesh& v) {
        return vecVecToArray(v.textureCoordinates3D());
      },
      [](ZMesh& v, const nb::ndarray<float>& array) {
        if (!zpy::isCPU(array)) {
          throw ZException("textureCoordinates3D ndarray must be CPU-backed");
        }
        v.setTextureCoordinates(arrayToVecVec<3>(array));
      })
    .def("__repr__", [](const ZMesh& v) {
      return fmt::format("<_imgpy.ZMesh {}>", v.toString());
    });

  m.attr("__version__") = GIT_VERSION;
}
