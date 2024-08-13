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
    EXPECT_THROW(enumToString(static_cast<TEnum>(999)), nim::ZException);      \
  }                                                                            \
  TEST(EnumConversionTests, EnumType##InvalidString)                           \
  {                                                                            \
    using TEnum = EnumType;                                                    \
    EXPECT_THROW(stringToEnum<TEnum>("InvalidString"), nim::ZException);       \
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

namespace nim {

enum class PositionHint
{
  None = 0, // no hint, any position can be possible
  Left = 1, // img is in left side, only overlap with left part of another img
  Right = 1 << 1, // img is in right side, only overlap with right part of another img
  Up = 1 << 2,
  Down = 1 << 3,
  Front = 1 << 4,
  Back = 1 << 5
};

DECLARE_OPERATORS_FOR_ENUM(PositionHint)

} // namespace nim

TEST(FlagsToStringTest, NoFlagSet)
{
  using namespace nim;
  EXPECT_EQ(flagsToString(PositionHint::None), "None");
}

TEST(FlagsToStringTest, SingleFlagSet)
{
  EXPECT_EQ(flagsToString(PositionHint::Left), "Left");
  EXPECT_EQ(flagsToString(PositionHint::Right), "Right");
  EXPECT_EQ(flagsToString(PositionHint::Up), "Up");
  EXPECT_EQ(flagsToString(PositionHint::Down), "Down");
  EXPECT_EQ(flagsToString(PositionHint::Front), "Front");
  EXPECT_EQ(flagsToString(PositionHint::Back), "Back");
}

TEST(FlagsToStringTest, MultipleFlagsSet)
{
  EXPECT_EQ(flagsToString(PositionHint(PositionHint::Left | PositionHint::Down)), "Left | Down");
  EXPECT_EQ(flagsToString(PositionHint(PositionHint::Right | PositionHint::Up)), "Right | Up");
  EXPECT_EQ(flagsToString(PositionHint(PositionHint::Front | PositionHint::Back)), "Front | Back");
  EXPECT_EQ(flagsToString(PositionHint(PositionHint::Left | PositionHint::Right | PositionHint::Up)),
            "Left | Right | Up");
  EXPECT_EQ(flagsToString(PositionHint::Left | PositionHint::Up | PositionHint::Front), "Left | Up | Front");
  EXPECT_EQ(flagsToString(PositionHint::Right | PositionHint::Down | PositionHint::Back), "Right | Down | Back");
}

TEST(FlagsToStringTest, AllFlagsSet)
{
  EXPECT_EQ(flagsToString(PositionHint(PositionHint::Left | PositionHint::Right | PositionHint::Up |
                                       PositionHint::Down | PositionHint::Front | PositionHint::Back)),
            "Left | Right | Up | Down | Front | Back");
}

TEST(FlagsToStringTest, NonAdjacentFlags)
{
  EXPECT_EQ(flagsToString(PositionHint::Left | PositionHint::Front), "Left | Front");
  EXPECT_EQ(flagsToString(PositionHint::Right | PositionHint::Up | PositionHint::Back), "Right | Up | Back");
}

TEST(FlagsToStringTest, CombinationWithNone)
{
  EXPECT_EQ(flagsToString(PositionHint(PositionHint::None | PositionHint::Left)), "Left");
  EXPECT_EQ(flagsToString(PositionHint(PositionHint::None | PositionHint::Right | PositionHint::Down)), "Right | Down");
}

TEST(FlagsToStringTest, OrderIndependence)
{
  auto flags1 = PositionHint::Left | PositionHint::Up;
  auto flags2 = PositionHint::Up | PositionHint::Left;
  EXPECT_EQ(flagsToString(flags1), flagsToString(flags2));
}

TEST(FlagsToStringTest, InvalidFlag)
{
  // Assuming your function handles invalid flags gracefully
  EXPECT_EQ(flagsToString(static_cast<PositionHint>(1 << 6)), "");
}
