#include "../version/version.h"
#include "typecast.h"
#include "zimg.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

using namespace nim;

using namespace pybind11::literals;

PYBIND11_MODULE(_imgpy, m)
{
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

  py::enum_<Dimension>(m, "Dimension", py::arithmetic())
    .value("X", Dimension::X)
    .value("Y", Dimension::Y)
    .value("Z", Dimension::Z)
    .value("C", Dimension::C)
    .value("T", Dimension::T);

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
    .value("JBIG", Compression::JBIG)
    .value("JP2000", Compression::JP2000);

  py::class_<col4>(m, "col4")
    .def(py::init<>())
    .def(py::init<uint8_t, uint8_t, uint8_t, uint8_t>(),
      "r"_a, "g"_a, "b"_a, "a"_a = 255_u8)
    .def_readwrite("r", &col4::r)
    .def_readwrite("g", &col4::g)
    .def_readwrite("b", &col4::b)
    .def_readwrite("a", &col4::a)
    .def("__init__", [](col4 &self, py::tuple t) {
      if (py::len(t) != 4)
        throw std::runtime_error("col4 needs tuple with 4 values");
      new(&self) col4(t[0].cast<uint8_t>(), t[1].cast<uint8_t>(), t[2].cast<uint8_t>(), t[3].cast<uint8_t>());
    })
    .def("__repr__", [](const col4& v) {
      return QString("<_imgpy.col4 r:%1, g:%2, b:%3, a:%4>").arg(v.r).arg(v.g).arg(v.b).arg(v.a).toStdString();
    });
  py::implicitly_convertible<py::tuple, col4>();

  static py::exception<ZException> base_ex(m, "ZException");
  static py::exception<ZIOException> io_ex(m, "ZIOException", base_ex.ptr());
  static py::exception<ZImgException> img_ex(m, "ZImgException", base_ex.ptr());
  static py::exception<ZProcessAbortException> pa_ex(m, "ZProcessAbortException", base_ex.ptr());
  static py::exception<ZGLException> gl_ex(m, "ZGLException", base_ex.ptr());
  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p) std::rethrow_exception(p);
    }
    catch (const ZIOException &e) {
      io_ex(qUtf8Printable(e.what()));
    }
    catch (const ZImgException &e) {
      img_ex(qUtf8Printable(e.what()));
    }
    catch (const ZProcessAbortException &e) {
      pa_ex(qUtf8Printable(e.what()));
    }
    catch (const ZGLException &e) {
      gl_ex(qUtf8Printable(e.what()));
    }
    catch (const ZException &e) {
      base_ex(qUtf8Printable(e.what()));
    }
  });

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
    .def("__repr__", [](const ZImgInfo& v) {
      return QString("<_imgpy.ZImgInfo %1>").arg(v.toQString()).toStdString();
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
    .def("__init__", [](ZVoxelCoordinate &self, py::tuple t) {
      if (py::len(t) != 5)
        throw std::runtime_error("ZVoxelCoordinate needs tuple with 5 values");
      new(&self) ZVoxelCoordinate(t[0].cast<ZVoxelCoordinate::value_type>(), t[1].cast<ZVoxelCoordinate::value_type>(),
        t[2].cast<ZVoxelCoordinate::value_type>(), t[3].cast<ZVoxelCoordinate::value_type>(),
        t[4].cast<ZVoxelCoordinate::value_type>());
    })
    .def("__repr__", [](const ZVoxelCoordinate& v) {
      return QString("<_imgpy.ZVoxelCoordinate xyzct:%1>").arg(v.toQString()).toStdString();
    });
  py::implicitly_convertible<py::tuple, ZVoxelCoordinate>();

  py::class_<ZImgRegion>(m, "ZImgRegion")
    .def(py::init<>())
    .def(py::init<ZVoxelCoordinate, ZVoxelCoordinate>(), "start"_a, "end"_a)
    .def_readwrite("start", &ZImgRegion::start)
    .def_readwrite("end", &ZImgRegion::end)
    .def("__repr__", [](const ZImgRegion& v) {
      return QString("<_imgpy.ZImgRegion %1>").arg(v.toQString()).toStdString();
    });

  py::class_<ZImgSource>(m, "ZImgSource")
    .def(py::init<>())
    .def(py::init<const QString&, const ZImgRegion&, size_t, FileFormat>(),
      "filename"_a, "region"_a = ZImgRegion(), "scene"_a = 0, "format"_a = FileFormat::Unknown)
    .def(py::init<>([](const std::list<QString>& fns, Dimension catDim, const ZImgRegion& rgn, size_t scene,
                       FileFormat format, bool expandXY, bool expandWithMaxValue) {
      return new ZImgSource(QStringList::fromStdList(fns), catDim, rgn, scene, format, expandXY, expandWithMaxValue);
                    }),
      "filenames"_a, "catDim"_a, "region"_a = ZImgRegion(), "scene"_a = 0, "format"_a = FileFormat::Unknown,
      "expandXY"_a = false, "expandWithMaxValue"_a = false)
    .def_property("filenames",
      [](const ZImgSource& v) {
        return v.filenames.toStdList();
      },
      [](ZImgSource& v, const std::list<QString>& fns) {
        v.filenames = QStringList::fromStdList(fns);
      })
    .def_readwrite("catDim", &ZImgSource::catDim)
    .def_readwrite("region", &ZImgSource::region)
    .def_readwrite("scene", &ZImgSource::scene)
    .def_readwrite("format", &ZImgSource::format)
    .def_readwrite("expandXY", &ZImgSource::expandXY)
    .def_readwrite("expandWithMaxValue", &ZImgSource::expandWithMaxValue)
    .def_readwrite("totalFileSize", &ZImgSource::totalFileSize)
    .def("__repr__", [](const ZImgSource& v) {
      return QString("<_imgpy.ZImgSource %1>").arg(v.toQString()).toStdString();
    });

  py::class_<ZImg>(m, "ZImg")
    .def(py::init<>())
    .def(py::init<const ZImgInfo&>())
    .def(py::init<const QString&, ZImgRegion, size_t, FileFormat>(),
      "filename"_a, "region"_a = ZImgRegion(), "scene"_a = 0, "format"_a = FileFormat::Unknown)
    .def(py::init<>([](const QStringList& fileList, Dimension catDim, const ZImgRegion& region, size_t scene,
                       FileFormat format, bool expandXY, bool expandWithMaxValue) {
      return new ZImg(fileList, catDim, region, scene, format, expandXY, expandWithMaxValue);
                    }),
      "filenames"_a, "catDim"_a, "region"_a = ZImgRegion(), "scene"_a = 0, "format"_a = FileFormat::Unknown,
      "expandXY"_a = false, "expandWithMaxValue"_a = false)
    .def(py::init<const ZImgSource&>())
    .def_static("readImgInfo", [](const QString& filename, FileFormat format) {
      return ZImg::readImgInfo(filename, nullptr, format);
      }, "filename"_a, "format"_a = FileFormat::Unknown)
    .def_static("readImgInfo", [](const QStringList& fileList, Dimension catDim, FileFormat format, bool expandXY) {
      return ZImg::readImgInfo(fileList, catDim, nullptr, format, expandXY);
      }, "filenames"_a, "catDim"_a, "format"_a = FileFormat::Unknown, "expandXY"_a = false)
    .def_static("readImgInfo", [](const ZImgSource& imgSource) {
      return ZImg::readImgInfo(imgSource);
      })
    .def("save", &ZImg::save, "format"_a = FileFormat::Unknown, "compression"_a = Compression::AUTO)
    .def_property("info",
      [](const ZImg& v) {
        return v.info();
      },
      [](ZImg& v, const ZImgInfo& info) {
        v.infoRef() = info;
      })
    .def_property("data",
      [](const ZImg& v) {

      },
      [](ZImg& v, py::array arr) {

      })
    .def("__repr__", [](const ZImg& v) {
      return QString("<_imgpy.ZImg %1>").arg(v.info().toQString()).toStdString();
    });

  m.attr("__version__") = GIT_VERSION;
}
