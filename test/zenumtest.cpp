#include "zimginterface.h"
#include "ztest.h"

using namespace nim;

// Helper macro to define test cases for each enum type
#define TEST_ENUM_CONVERSIONS(EnumType, ...)                                   \
  TEST(EnumConversionTests, EnumType##ToStringAndBack)                         \
  {                                                                            \
    using TEnum = EnumType;                                                    \
    std::vector<std::pair<TEnum, std::string_view>> testCases = {__VA_ARGS__}; \
    for (const auto& [enumValue, strValue] : testCases) {                      \
      EXPECT_EQ(enumToString(enumValue), strValue);                            \
      EXPECT_EQ(stringToEnum<TEnum>(strValue), enumValue);                     \
    }                                                                          \
  }                                                                            \
  TEST(EnumConversionTests, EnumType##InvalidEnum)                             \
  {                                                                            \
    using TEnum = EnumType;                                                    \
    EXPECT_THROW(enumToString(static_cast<TEnum>(999)), nim::ZIOException);    \
  }                                                                            \
  TEST(EnumConversionTests, EnumType##InvalidString)                           \
  {                                                                            \
    using TEnum = EnumType;                                                    \
    EXPECT_THROW(stringToEnum<TEnum>("InvalidString"), nim::ZIOException);     \
  }

// Define test cases for each enum type
TEST_ENUM_CONVERSIONS(DataType,
                      {DataType::Byte, "Byte"},
                      {DataType::Ascii, "Ascii"},
                      {DataType::Short, "Short"},
                      {DataType::Long, "Long"},
                      {DataType::Rational, "Rational"},
                      {DataType::SByte, "SByte"},
                      {DataType::Undefined, "Undefined"},
                      {DataType::SShort, "SShort"},
                      {DataType::SLong, "SLong"},
                      {DataType::SRational, "SRational"},
                      {DataType::Float, "Float"},
                      {DataType::Double, "Double"},
                      {DataType::IFD, "IFD"},
                      {DataType::Long8, "Long8"},
                      {DataType::SLong8, "SLong8"},
                      {DataType::IFD8, "IFD8"});

TEST_ENUM_CONVERSIONS(VoxelFormat,
                      {VoxelFormat::Unsigned, "Unsigned"},
                      {VoxelFormat::Signed, "Signed"},
                      {VoxelFormat::Float, "Float"});

TEST_ENUM_CONVERSIONS(VoxelSizeUnit,
                      {VoxelSizeUnit::none, "none"},
                      {VoxelSizeUnit::inch, "inch"},
                      {VoxelSizeUnit::cm, "cm"},
                      {VoxelSizeUnit::mm, "mm"},
                      {VoxelSizeUnit::um, "um"},
                      {VoxelSizeUnit::nm, "nm"},
                      {VoxelSizeUnit::m, "m"},
                      {VoxelSizeUnit::hm, "hm"},
                      {VoxelSizeUnit::km, "km"});

TEST_ENUM_CONVERSIONS(FileFormat,
                      {FileFormat::Unknown, "Unknown"},
                      {FileFormat::HDF5Img, "HDF5Img"},
                      {FileFormat::OmeTiff, "OmeTiff"},
                      {FileFormat::Tiff, "Tiff"},
                      {FileFormat::Vaa3DRaw, "Vaa3DRaw"},
                      {FileFormat::ZeissLsm, "ZeissLsm"},
                      {FileFormat::Jpeg, "Jpeg"},
                      {FileFormat::JpegXR, "JpegXR"},
                      {FileFormat::Png, "Png"},
                      {FileFormat::FreeImage, "FreeImage"},
                      {FileFormat::MetaImage, "MetaImage"},
                      {FileFormat::ZeissCZI, "ZeissCZI"},
                      {FileFormat::ITKImage, "ITKImage"},
                      {FileFormat::Leica, "Leica"});

TEST_ENUM_CONVERSIONS(Compression,
                      {Compression::AUTO, "AUTO"},
                      {Compression::NONE, "NONE"},
                      {Compression::LZW, "LZW"},
                      {Compression::JPEG, "JPEG"},
                      {Compression::T85, "T85"},
                      {Compression::T43, "T43"},
                      {Compression::PACKBITS, "PACKBITS"},
                      {Compression::DEFLATE, "DEFLATE"},
                      {Compression::ADOBE_DEFLATE, "ADOBE_DEFLATE"},
                      {Compression::DCS, "DCS"},
                      {Compression::JP2000, "JP2000"},
                      {Compression::LZMA, "LZMA"},
                      {Compression::ZSTD, "ZSTD"},
                      {Compression::WEBP, "WEBP"},
                      {Compression::JPEGXR, "JPEGXR"});

TEST_ENUM_CONVERSIONS(PadOption,
                      {PadOption::Constant, "Constant"},
                      {PadOption::Symmetric, "Symmetric"},
                      {PadOption::Replicate, "Replicate"},
                      {PadOption::Circular, "Circular"});

TEST_ENUM_CONVERSIONS(Interpolant,
                      {Interpolant::Nearest, "Nearest"},
                      {Interpolant::Linear, "Linear"},
                      {Interpolant::Cubic, "Cubic"},
                      {Interpolant::Lanczos2, "Lanczos2"},
                      {Interpolant::Lanczos3, "Lanczos3"});

TEST_ENUM_CONVERSIONS(Dimension,
                      {Dimension::X, "X"},
                      {Dimension::Y, "Y"},
                      {Dimension::Z, "Z"},
                      {Dimension::C, "C"},
                      {Dimension::T, "T"});

TEST_ENUM_CONVERSIONS(ImgMergeMode,
                      {ImgMergeMode::Max, "Max"},
                      {ImgMergeMode::Min, "Min"},
                      {ImgMergeMode::Mean, "Mean"},
                      {ImgMergeMode::Median, "Median"},
                      {ImgMergeMode::First, "First"},
                      {ImgMergeMode::Interpolation, "Interpolation"});
