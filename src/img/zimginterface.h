#pragma once

#include "zglobal.h"
#include "zjson.h"

namespace nim {

// same as tif
enum class DataType : std::uint16_t
{
  Byte = 1,  // 8-bit unsigned integer.
  Ascii = 2,  // 8-bit byte that contains a 7-bit ASCII code or utf-8 string; the last byte must be NUL (binary zero).
  Short = 3,  // 16-bit (2-byte) unsigned integer.
  Long = 4,  // 32-bit (4-byte) unsigned integer.
  Rational = 5,  // Two LONGs: the first represents the numerator of a fraction; the second, the denominator.
  SByte = 6,  // An 8-bit signed (twos-complement) integer.
  Undefined = 7,  // An 8-bit byte that may contain anything, depending on the definition of the field.
  SShort = 8,  // A 16-bit (2-byte) signed (twos-complement) integer.
  SLong = 9,  // A 32-bit (4-byte) signed (twos-complement) integer.
  SRational = 10, // Two SLONG’s: the first represents the numerator of a fraction, the second the denominator.
  Float = 11, // Single precision (4-byte) IEEE format.
  Double = 12, // Double precision (8-byte) IEEE format.
  IFD = 13, // %32-bit unsigned integer (offset)
  Long8 = 16, // BigTIFF 64-bit unsigned integer
  SLong8 = 17, // BigTIFF 64-bit signed integer
  IFD8 = 18  // BigTIFF 64-bit unsigned integer (offset)
};

inline QString enumToString(DataType dt)
{
  switch (dt) {
    case DataType::Byte:
      return QStringLiteral("Byte");
    case DataType::Ascii:
      return QStringLiteral("Ascii");
    case DataType::Short:
      return QStringLiteral("Short");
    case DataType::Long:
      return QStringLiteral("Long");
    case DataType::Rational:
      return QStringLiteral("Rational");
    case DataType::SByte:
      return QStringLiteral("SByte");
    case DataType::Undefined:
      return QStringLiteral("Undefined");
    case DataType::SShort:
      return QStringLiteral("SShort");
    case DataType::SLong:
      return QStringLiteral("SLong");
    case DataType::SRational:
      return QStringLiteral("SRational");
    case DataType::Float:
      return QStringLiteral("Float");
    case DataType::Double:
      return QStringLiteral("Double");
    case DataType::IFD:
      return QStringLiteral("IFD");
    case DataType::Long8:
      return QStringLiteral("Long8");
    case DataType::SLong8:
      return QStringLiteral("SLong8");
    case DataType::IFD8:
      return QStringLiteral("IFD8");
    default:
      throw ZIOException("invalid DataType");
  }
}

inline bool isValidDataType(uint32_t dataType)
{
  return dataType >= 1 && dataType <= 18 && dataType != 14 && dataType != 15;
}

size_t byteNumber(DataType dt);

// Data storage format used along with pixel depth (BPP)
// same as tif
enum class VoxelFormat : std::uint16_t
{
  Unsigned = 1,  // unsigned integer data
  Signed = 2,  // two’s complement signed integer data
  Float = 3   // IEEE floating point data [IEEE]
};

inline QString enumToString(VoxelFormat vf)
{
  switch (vf) {
    case VoxelFormat::Unsigned:
      return QStringLiteral("Unsigned");
    case VoxelFormat::Signed:
      return QStringLiteral("Signed");
    case VoxelFormat::Float:
      return QStringLiteral("Float");
    default:
      throw ZIOException("invalid VoxelFormat");
  }
}

inline VoxelFormat stringToVoxelFormat(const QString& str)
{
  if (str == QStringLiteral("Unsigned")) {
    return VoxelFormat::Unsigned;
  } else if (str == QStringLiteral("Signed")) {
    return VoxelFormat::Signed;
  } else if (str == QStringLiteral("Float")) {
    return VoxelFormat::Float;
  } else {
    throw ZIOException("invalid VoxelFormat " + str);
  }
}

enum class VoxelSizeUnit
{
  none = 0,  // No unit
  inch = 1,  // Inch = 0.0254 m
  cm = 2,  // Centi Meter 1e-2 m
  mm = 3,  // Mili Meter 1e-3 m
  um = 4,  // Micro Meter 1e-6 m
  nm = 5,  // Nano Meter 1e-9 m
  m = 6,  // Meter 1 m
  hm = 7,  // Hecto Meter 1e2 m
  km = 8   // Kilo Meters 1e3 m
};

inline QString enumToString(VoxelSizeUnit vsu)
{
  switch (vsu) {
    case VoxelSizeUnit::none:
      return QStringLiteral("none");
    case VoxelSizeUnit::inch:
      return QStringLiteral("inch");
    case VoxelSizeUnit::cm:
      return QStringLiteral("cm");
    case VoxelSizeUnit::mm:
      return QStringLiteral("mm");
    case VoxelSizeUnit::um:
      return QStringLiteral("um");
    case VoxelSizeUnit::nm:
      return QStringLiteral("nm");
    case VoxelSizeUnit::m:
      return QStringLiteral("m");
    case VoxelSizeUnit::hm:
      return QStringLiteral("hm");
    case VoxelSizeUnit::km:
      return QStringLiteral("km");
    default:
      throw ZIOException("invalid VoxelSizeUnit");
  }
}

inline VoxelSizeUnit stringToVoxelSizeUnit(const QString& str)
{
  if (str == QStringLiteral("none")) {
    return VoxelSizeUnit::none;
  } else if (str == QStringLiteral("inch")) {
    return VoxelSizeUnit::inch;
  } else if (str == QStringLiteral("cm")) {
    return VoxelSizeUnit::cm;
  } else if (str == QStringLiteral("mm")) {
    return VoxelSizeUnit::mm;
  } else if (str == QStringLiteral("um")) {
    return VoxelSizeUnit::um;
  } else if (str == QStringLiteral("nm")) {
    return VoxelSizeUnit::nm;
  } else if (str == QStringLiteral("m")) {
    return VoxelSizeUnit::m;
  } else if (str == QStringLiteral("hm")) {
    return VoxelSizeUnit::hm;
  } else if (str == QStringLiteral("km")) {
    return VoxelSizeUnit::km;
  } else {
    throw ZIOException("invalid VoxelSizeUnit " + str);
  }
}

double unitSizeInMeter(VoxelSizeUnit vsu);

#pragma pack(push, 1)

struct col4
{
  using value_type = uint8_t;

  value_type r, g, b, a;

  col4()
    : r(0), g(0), b(0), a(255)
  {}

  template<typename A>
  explicit col4(A v)
    : r(v), g(v), b(v), a(v)
  {}

  col4(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255_u8)
    : r(r_), g(g_), b(b_), a(a_)
  {}

  template<typename A, typename B, typename C, typename D>
  col4(A r_, B g_, C b_, D a_ = D(255))
    : r(r_), g(g_), b(b_), a(a_)
  {}

  bool operator==(const col4& c) const
  { return (r == c.r && g == c.g && b == c.b && a == c.a); }

  [[nodiscard]] float redF() const
  { return float(r) / 255.f; }

  [[nodiscard]] float greenF() const
  { return float(g) / 255.f; }

  [[nodiscard]] float blueF() const
  { return float(b) / 255.f; }

  [[nodiscard]] float alphaF() const
  { return float(a) / 255.f; }

  col4& max(const col4& c)
  {
    if (c.r > r) r = c.r;
    if (c.g > g) g = c.g;
    if (c.b > b) b = c.b;
    if (c.a > a) a = c.a;
    return *this;
  }

  static col4 max(const col4& c1, const col4& c2)
  {
    col4 res = c1;
    return res.max(c2);
  }

  template<typename FloatType>
  col4& mix(const col4& c, FloatType coef)
  {
    if (coef > FloatType(1)) {
      *this = c;
    } else if (coef > FloatType(0)) {
      r = static_cast<uint8_t>((FloatType(1) - coef) * r + coef * c.r);
      g = static_cast<uint8_t>((FloatType(1) - coef) * g + coef * c.g);
      b = static_cast<uint8_t>((FloatType(1) - coef) * b + coef * c.b);
      a = static_cast<uint8_t>((FloatType(1) - coef) * a + coef * c.a);
    }
    return *this;
  }

  template<typename FloatType>
  static col4 mix(const col4& c1, const col4& c2, FloatType coef)
  {
    col4 res = c1;
    return res.mix(c2, coef);
  }

  // access
  inline value_type& operator[](size_t i)
  { return (&r)[i]; }

  inline const value_type& operator[](size_t i) const
  { return (&r)[i]; }

  [[nodiscard]] inline QString toQString() const
  { return jsonToQString(*this); }

  [[nodiscard]] inline std::string toString() const
  { return jsonToString(*this); }
};

struct Location
{
  Location()
    : x(0), y(0), z(0)
  {}

  Location(double x_, double y_, double z_)
    : x(x_), y(y_), z(z_)
  {}

  bool operator==(const Location& l) const
  { return (x == l.x && y == l.y && z == l.z); }

  double x, y, z;
};

#pragma pack(pop)

enum class FileFormat
{
  Unknown = 0,
  HDF5Img,
  OmeTiff,
  Tiff,
  Vaa3DRaw,
  ZeissLsm,
  Jpeg,
  JpegXR,
  Png,
  FreeImage,
  MetaImage,
  ZeissCZI,
  ITKImage,
  Leica,
};

inline QString enumToString(FileFormat f)
{
  switch (f) {
    case FileFormat::Unknown:
      return QStringLiteral("Unknown");
    case FileFormat::HDF5Img:
      return QStringLiteral("HDF5Img");
    case FileFormat::OmeTiff:
      return QStringLiteral("OmeTiff");
    case FileFormat::Tiff:
      return QStringLiteral("Tiff");
    case FileFormat::Vaa3DRaw:
      return QStringLiteral("Vaa3DRaw");
    case FileFormat::ZeissLsm:
      return QStringLiteral("ZeissLsm");
    case FileFormat::Jpeg:
      return QStringLiteral("Jpeg");
    case FileFormat::JpegXR:
      return QStringLiteral("JpegXR");
    case FileFormat::Png:
      return QStringLiteral("Png");
    case FileFormat::FreeImage:
      return QStringLiteral("FreeImage");
    case FileFormat::MetaImage:
      return QStringLiteral("MetaImage");
    case FileFormat::ZeissCZI:
      return QStringLiteral("ZeissCZI");
    case FileFormat::ITKImage:
      return QStringLiteral("ITKImage");
    case FileFormat::Leica:
      return QStringLiteral("Leica");
    default:
      throw ZIOException("invalid FileFormat");
  }
}

inline FileFormat stringToFileFormat(const QString& str)
{
  if (str == QStringLiteral("Unknown")) {
    return FileFormat::Unknown;
  } else if (str == QStringLiteral("HDF5Img")) {
    return FileFormat::HDF5Img;
  } else if (str == QStringLiteral("OmeTiff")) {
    return FileFormat::OmeTiff;
  } else if (str == QStringLiteral("Tiff")) {
    return FileFormat::Tiff;
  } else if (str == QStringLiteral("Vaa3DRaw")) {
    return FileFormat::Vaa3DRaw;
  } else if (str == QStringLiteral("ZeissLsm")) {
    return FileFormat::ZeissLsm;
  } else if (str == QStringLiteral("Jpeg")) {
    return FileFormat::Jpeg;
  } else if (str == QStringLiteral("JpegXR")) {
    return FileFormat::JpegXR;
  } else if (str == QStringLiteral("Png")) {
    return FileFormat::Png;
  } else if (str == QStringLiteral("FreeImage")) {
    return FileFormat::FreeImage;
  } else if (str == QStringLiteral("MetaImage")) {
    return FileFormat::MetaImage;
  } else if (str == QStringLiteral("ZeissCZI")) {
    return FileFormat::ZeissCZI;
  } else if (str == QStringLiteral("ITKImage")) {
    return FileFormat::ITKImage;
  } else if (str == QStringLiteral("Leica")) {
    return FileFormat::Leica;
  } else {
    throw ZIOException("invalid FileFormat " + str);
  }
}

enum class Compression : std::uint16_t
{
  AUTO = 0, //choose based on image type
  NONE = 1,  /* dump mode */
  LZW = 5,       /* Lempel-Ziv  & Welch */
  JPEG = 7,  /* %JPEG DCT compression */  // lossy [default for YCBCR]
  T85 = 9,  /* !TIFF/FX T.85 JBIG compression */
  T43 = 10,  /* !TIFF/FX T.43 colour by layered JBIG compression */
  PACKBITS = 32773,  /* Macintosh RLE */ // [default for MAP]

  DEFLATE = 32946,  /* Deflate compression */ // zip
  ADOBE_DEFLATE = 8,       /* Deflate compression,
               as recognized by Adobe */
  /* compression code 32947 is reserved for Oceana Matrix <dev@oceana.com> */
  DCS = 32947,   /* Kodak DCS encoding */
  JP2000 = 34712,   /* Leadtools JPEG2000 */
  /* compression codes 34887-34889 are reserved for ESRI */
  LZMA = 34925,  /* LZMA2 */
  ZSTD = 50000, /* ZSTD: WARNING not registered in Adobe-maintained registry */
  WEBP = 50001, /* WEBP: WARNING not registered in Adobe-maintained registry */
};

inline QString enumToString(Compression m)
{
  switch (m) {
    case Compression::AUTO:
      return QStringLiteral("Auto");
    case Compression::NONE:
      return QStringLiteral("None");
    case Compression::LZW:
      return QStringLiteral("LZW");
    case Compression::JPEG:
      return QStringLiteral("JPEG");
    case Compression::T85:
      return QStringLiteral("T85");
    case Compression::T43:
      return QStringLiteral("T43");
    case Compression::PACKBITS:
      return QStringLiteral("PACKBITS");
    case Compression::DEFLATE:
      return QStringLiteral("DEFLATE");
    case Compression::ADOBE_DEFLATE:
      return QStringLiteral("ADOBE_DEFLATE");
    case Compression::DCS:
      return QStringLiteral("DCS");
    case Compression::JP2000:
      return QStringLiteral("JP2000");
    case Compression::LZMA:
      return QStringLiteral("LZMA");
    case Compression::ZSTD:
      return QStringLiteral("ZSTD");
    case Compression::WEBP:
      return QStringLiteral("WEBP");
    default:
      throw ZIOException("invalid Compression");
  }
}

enum class PadOption
{
  //Input array values outside the bounds of the array are implicitly
  //assumed to have the value boundaryValue.
    Constant = 0,
  //Input array values outside the bounds of the array are computed by
  //mirror-reflecting the array across the array border.
    Symmetric = 1,
  //Input array values outside the bounds of the array are assumed to
  //equal the nearest array border value.
    Replicate = 2,
  //Input array values outside the bounds of the array are computed by
  //implicitly assuming the input array is periodic.
    Circular = 3
};

enum class Interpolant
{
  Nearest,
  Linear,
  Cubic,
  Lanczos2,
  Lanczos3
};

enum class Dimension
{
  X = 0,
  Y = 1,
  Z = 2,
  C = 3,
  T = 4
};

inline QString enumToString(Dimension d)
{
  switch (d) {
    case Dimension::X:
      return QStringLiteral("X");
    case Dimension::Y:
      return QStringLiteral("Y");
    case Dimension::Z:
      return QStringLiteral("Z");
    case Dimension::C:
      return QStringLiteral("C");
    case Dimension::T:
      return QStringLiteral("T");
    default:
      throw ZIOException("invalid Dimension");
  }
}

inline Dimension stringToDimension(const QString& str)
{
  if (str == QStringLiteral("X")) {
    return Dimension::X;
  } else if (str == QStringLiteral("Y")) {
    return Dimension::Y;
  } else if (str == QStringLiteral("Z")) {
    return Dimension::Z;
  } else if (str == QStringLiteral("C")) {
    return Dimension::C;
  } else if (str == QStringLiteral("T")) {
    return Dimension::T;
  } else {
    throw ZIOException("invalid Dimension " + str);
  }
}

enum class ImgMergeMode
{
  Max, Min, Mean, Median, First, Interpolation
};

inline QString enumToString(ImgMergeMode m)
{
  switch (m) {
    case ImgMergeMode::Max:
      return QStringLiteral("Max");
    case ImgMergeMode::Min:
      return QStringLiteral("Min");
    case ImgMergeMode::Mean:
      return QStringLiteral("Mean");
    case ImgMergeMode::Median:
      return QStringLiteral("Median");
    case ImgMergeMode::First:
      return QStringLiteral("First");
    default:
      throw ZIOException("invalid ImgMergeMode");
  }
}

inline ImgMergeMode stringToImgMergeMode(const QString& str)
{
  if (str == QStringLiteral("Max")) {
    return ImgMergeMode::Max;
  } else if (str == QStringLiteral("Min")) {
    return ImgMergeMode::Min;
  } else if (str == QStringLiteral("Mean")) {
    return ImgMergeMode::Mean;
  } else if (str == QStringLiteral("Median")) {
    return ImgMergeMode::Median;
  } else if (str == QStringLiteral("First")) {
    return ImgMergeMode::First;
  } else {
    throw ZIOException("invalid ImgMergeMode " + str);
  }
}

template<std::size_t Index>
constexpr auto&& get(col4& v) noexcept
{ return tuple_like_get_helper<Index, 4>(v); }

template<std::size_t Index>
constexpr auto&& get(const col4& v) noexcept
{ return tuple_like_get_helper<Index, 4>(v); }

template<std::size_t Index>
constexpr auto&& get(col4&& v) noexcept
{ return tuple_like_get_helper<Index, 4>(v); }

template<std::size_t Index>
constexpr auto&& get(const col4&& v) noexcept
{ return tuple_like_get_helper<Index, 4>(v); }

} // namespace nim

namespace std {

template<>
struct tuple_size<nim::col4> : integral_constant<size_t, 4>
{
};

template<std::size_t Index>
struct tuple_element<Index, nim::col4>
{
  static_assert(Index < 4, "Index out of bounds for col4");
  using type = nim::col4::value_type;
};

} // namespace std

