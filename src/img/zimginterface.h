#pragma once

#include "zglobal.h"

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

//
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

double unitSizeInMeter(VoxelSizeUnit vsu);

#pragma pack(push, 1)

struct col4
{
  uint8_t r, g, b, a;

  col4()
    : r(0), g(0), b(0), a(255)
  {}

  template<typename A>
  col4(A v)
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
};

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
  JBIG = 34661,  /* ISO JBIG */
  JP2000 = 34712   /* Leadtools JPEG2000 */
};

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

}

