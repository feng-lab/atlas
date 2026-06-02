#include "zexception.h"
#include "zimg.h"
#include "zimgopenimageio.h"

#include <OpenImageIO/imageio.h>
#include <gif_lib.h>
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <array>
#include <vector>

namespace nim {
namespace {

constexpr int kAnimatedGifWidth = 3;
constexpr int kAnimatedGifHeight = 2;
constexpr int kAnimatedGifChannels = 4;

[[nodiscard]] std::string oiioTestFilename(const QString& filename)
{
#if defined(_WIN32) || defined(_WIN64)
  return filename.toStdString();
#else
  return QFile::encodeName(filename).toStdString();
#endif
}

[[nodiscard]] std::vector<uint8_t> makeSolidRgbaFrame(uint8_t red, uint8_t green, uint8_t blue)
{
  std::vector<uint8_t> frame(kAnimatedGifWidth * kAnimatedGifHeight * kAnimatedGifChannels);
  for (int pixel = 0; pixel < kAnimatedGifWidth * kAnimatedGifHeight; ++pixel) {
    frame[pixel * kAnimatedGifChannels + 0] = red;
    frame[pixel * kAnimatedGifChannels + 1] = green;
    frame[pixel * kAnimatedGifChannels + 2] = blue;
    frame[pixel * kAnimatedGifChannels + 3] = 255;
  }
  return frame;
}

[[nodiscard]] QString writeRgbPngFixture(const QString& dirPath)
{
  ZImgInfo info(5, 4, 1, 3, 1, 1, VoxelFormat::Unsigned);
  ZImg img(info);

  for (size_t c = 0; c < img.numChannels(); ++c) {
    auto* channel = img.channelData<uint8_t>(c);
    for (size_t y = 0; y < img.height(); ++y) {
      for (size_t x = 0; x < img.width(); ++x) {
        channel[y * img.width() + x] = static_cast<uint8_t>(c * 50 + y * img.width() + x);
      }
    }
  }

  const QString path = QDir(dirPath).filePath(QStringLiteral("oiio_fixture.png"));
  img.save(path, FileFormat::Png);
  return path;
}

void writeAnimatedGifFixture(const QString& path)
{
  std::array<std::vector<uint8_t>, 2> frames = {makeSolidRgbaFrame(255, 0, 0), makeSolidRgbaFrame(0, 255, 0)};
  OIIO::ImageSpec spec(kAnimatedGifWidth, kAnimatedGifHeight, kAnimatedGifChannels, OIIO::TypeUInt8);
  spec.attribute("FramesPerSecond", 2.0f);
  std::array<OIIO::ImageSpec, 2> specs = {spec, spec};

  const std::string filename = oiioTestFilename(path);
  auto output = OIIO::ImageOutput::create(filename);
  ASSERT_NE(output, nullptr);
  ASSERT_TRUE(output->open(filename, static_cast<int>(specs.size()), specs.data())) << output->geterror();
  ASSERT_TRUE(output->write_image(OIIO::TypeUInt8, frames[0].data())) << output->geterror();
  ASSERT_TRUE(output->open(filename, specs[1], OIIO::ImageOutput::AppendSubimage)) << output->geterror();
  ASSERT_TRUE(output->write_image(OIIO::TypeUInt8, frames[1].data())) << output->geterror();
  ASSERT_TRUE(output->close()) << output->geterror();
}

void writeDeltaGifFixture(const QString& path)
{
  ColorMapObject* colorMap = GifMakeMapObject(2, nullptr);
  ASSERT_NE(colorMap, nullptr);
  colorMap->Colors[0] = GifColorType{255, 0, 0};
  colorMap->Colors[1] = GifColorType{0, 255, 0};

  int error = 0;
  GifFileType* gif = EGifOpenFileName(oiioTestFilename(path).c_str(), false, &error);
  ASSERT_NE(gif, nullptr) << GifErrorString(error);
  EGifSetGifVersion(gif, true);
  ASSERT_EQ(EGifPutScreenDesc(gif, kAnimatedGifWidth, kAnimatedGifHeight, 2, 0, colorMap), GIF_OK)
    << GifErrorString(gif->Error);

  const GifByteType doNotDispose[] = {static_cast<GifByteType>(DISPOSE_DO_NOT << 2), 10, 0, 0};
  ASSERT_EQ(EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, 4, doNotDispose), GIF_OK) << GifErrorString(gif->Error);
  ASSERT_EQ(EGifPutImageDesc(gif, 0, 0, kAnimatedGifWidth, kAnimatedGifHeight, false, nullptr), GIF_OK)
    << GifErrorString(gif->Error);
  std::array<GifPixelType, kAnimatedGifWidth> redRow = {0, 0, 0};
  for (int y = 0; y < kAnimatedGifHeight; ++y) {
    ASSERT_EQ(EGifPutLine(gif, redRow.data(), kAnimatedGifWidth), GIF_OK) << GifErrorString(gif->Error);
  }

  ASSERT_EQ(EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, 4, doNotDispose), GIF_OK) << GifErrorString(gif->Error);
  ASSERT_EQ(EGifPutImageDesc(gif, 1, 0, 1, 1, false, nullptr), GIF_OK) << GifErrorString(gif->Error);
  GifPixelType greenPixel = 1;
  ASSERT_EQ(EGifPutLine(gif, &greenPixel, 1), GIF_OK) << GifErrorString(gif->Error);

  int closeError = 0;
  ASSERT_EQ(EGifCloseFile(gif, &closeError), GIF_OK) << GifErrorString(closeError);
  GifFreeMapObject(colorMap);
}

void writeTransparentGifFixture(const QString& path)
{
  ColorMapObject* colorMap = GifMakeMapObject(4, nullptr);
  ASSERT_NE(colorMap, nullptr);
  colorMap->Colors[0] = GifColorType{255, 0, 0};
  colorMap->Colors[1] = GifColorType{0, 255, 0};
  colorMap->Colors[2] = GifColorType{0, 0, 255};
  colorMap->Colors[3] = GifColorType{0, 0, 0};

  int error = 0;
  GifFileType* gif = EGifOpenFileName(oiioTestFilename(path).c_str(), false, &error);
  ASSERT_NE(gif, nullptr) << GifErrorString(error);
  EGifSetGifVersion(gif, true);
  ASSERT_EQ(EGifPutScreenDesc(gif, kAnimatedGifWidth, kAnimatedGifHeight, 2, 0, colorMap), GIF_OK)
    << GifErrorString(gif->Error);

  const GifByteType doNotDispose[] = {static_cast<GifByteType>(DISPOSE_DO_NOT << 2), 10, 0, 0};
  ASSERT_EQ(EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, 4, doNotDispose), GIF_OK) << GifErrorString(gif->Error);
  ASSERT_EQ(EGifPutImageDesc(gif, 0, 0, kAnimatedGifWidth, kAnimatedGifHeight, false, nullptr), GIF_OK)
    << GifErrorString(gif->Error);
  std::array<GifPixelType, kAnimatedGifWidth> redRow = {0, 0, 0};
  for (int y = 0; y < kAnimatedGifHeight; ++y) {
    ASSERT_EQ(EGifPutLine(gif, redRow.data(), kAnimatedGifWidth), GIF_OK) << GifErrorString(gif->Error);
  }

  const GifByteType transparentDoNotDispose[] = {static_cast<GifByteType>((DISPOSE_DO_NOT << 2) | 0x01), 10, 0, 2};
  ASSERT_EQ(EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, 4, transparentDoNotDispose), GIF_OK)
    << GifErrorString(gif->Error);
  ASSERT_EQ(EGifPutImageDesc(gif, 0, 0, kAnimatedGifWidth, kAnimatedGifHeight, false, nullptr), GIF_OK)
    << GifErrorString(gif->Error);
  std::array<GifPixelType, kAnimatedGifWidth> transparentRow = {2, 2, 2};
  std::array<GifPixelType, kAnimatedGifWidth> greenOverlayRow = {2, 1, 2};
  ASSERT_EQ(EGifPutLine(gif, greenOverlayRow.data(), kAnimatedGifWidth), GIF_OK) << GifErrorString(gif->Error);
  ASSERT_EQ(EGifPutLine(gif, transparentRow.data(), kAnimatedGifWidth), GIF_OK) << GifErrorString(gif->Error);

  int closeError = 0;
  ASSERT_EQ(EGifCloseFile(gif, &closeError), GIF_OK) << GifErrorString(closeError);
  GifFreeMapObject(colorMap);
}

void writeBackgroundDisposedGifFixture(const QString& path)
{
  ColorMapObject* colorMap = GifMakeMapObject(4, nullptr);
  ASSERT_NE(colorMap, nullptr);
  colorMap->Colors[0] = GifColorType{255, 0, 0};
  colorMap->Colors[1] = GifColorType{0, 255, 0};
  colorMap->Colors[2] = GifColorType{0, 0, 255};
  colorMap->Colors[3] = GifColorType{0, 0, 0};

  int error = 0;
  GifFileType* gif = EGifOpenFileName(oiioTestFilename(path).c_str(), false, &error);
  ASSERT_NE(gif, nullptr) << GifErrorString(error);
  EGifSetGifVersion(gif, true);
  ASSERT_EQ(EGifPutScreenDesc(gif, kAnimatedGifWidth, kAnimatedGifHeight, 2, 2, colorMap), GIF_OK)
    << GifErrorString(gif->Error);

  const GifByteType disposeBackground[] = {static_cast<GifByteType>(DISPOSE_BACKGROUND << 2), 10, 0, 0};
  ASSERT_EQ(EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, 4, disposeBackground), GIF_OK) << GifErrorString(gif->Error);
  ASSERT_EQ(EGifPutImageDesc(gif, 0, 0, kAnimatedGifWidth, kAnimatedGifHeight, false, nullptr), GIF_OK)
    << GifErrorString(gif->Error);
  std::array<GifPixelType, kAnimatedGifWidth> redRow = {0, 0, 0};
  for (int y = 0; y < kAnimatedGifHeight; ++y) {
    ASSERT_EQ(EGifPutLine(gif, redRow.data(), kAnimatedGifWidth), GIF_OK) << GifErrorString(gif->Error);
  }

  const GifByteType doNotDispose[] = {static_cast<GifByteType>(DISPOSE_DO_NOT << 2), 10, 0, 0};
  ASSERT_EQ(EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, 4, doNotDispose), GIF_OK) << GifErrorString(gif->Error);
  ASSERT_EQ(EGifPutImageDesc(gif, 1, 0, 1, 1, false, nullptr), GIF_OK) << GifErrorString(gif->Error);
  GifPixelType greenPixel = 1;
  ASSERT_EQ(EGifPutLine(gif, &greenPixel, 1), GIF_OK) << GifErrorString(gif->Error);

  int closeError = 0;
  ASSERT_EQ(EGifCloseFile(gif, &closeError), GIF_OK) << GifErrorString(closeError);
  GifFreeMapObject(colorMap);
}

TEST(ZImgOpenImageIO, ReadsPngInfoAndRegion)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = writeRgbPngFixture(tmp.path());

  std::vector<ZImgInfo> infos = ZImg::readImgInfos(path, nullptr, FileFormat::OpenImageIO);
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].width, 5u);
  EXPECT_EQ(infos[0].height, 4u);
  EXPECT_EQ(infos[0].depth, 1u);
  EXPECT_EQ(infos[0].numChannels, 3u);
  EXPECT_EQ(infos[0].numTimes, 1u);
  EXPECT_EQ(infos[0].bytesPerVoxel, 1u);
  EXPECT_EQ(infos[0].voxelFormat, VoxelFormat::Unsigned);

  const ZImgRegion region(1, 4, 1, 3, 0, 1, 1, 3);
  const ZImg img(path, region, 0, 1, 1, 1, FileFormat::OpenImageIO);
  ASSERT_EQ(img.width(), 3u);
  ASSERT_EQ(img.height(), 2u);
  ASSERT_EQ(img.depth(), 1u);
  ASSERT_EQ(img.numChannels(), 2u);

  for (size_t c = 0; c < img.numChannels(); ++c) {
    const size_t sourceChannel = c + 1;
    for (size_t y = 0; y < img.height(); ++y) {
      for (size_t x = 0; x < img.width(); ++x) {
        const size_t sourceX = x + 1;
        const size_t sourceY = y + 1;
        const uint8_t expected = static_cast<uint8_t>(sourceChannel * 50 + sourceY * 5 + sourceX);
        EXPECT_EQ(*img.data<uint8_t>(x, y, 0, c), expected);
      }
    }
  }
}

TEST(ZImgOpenImageIO, ReadsAnimatedGifAsTimeDimension)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = QDir(tmp.path()).filePath(QStringLiteral("oiio_animated.gif"));
  writeAnimatedGifFixture(path);
  ASSERT_TRUE(QFile::exists(path));

  std::vector<ZImgInfo> infos = ZImg::readImgInfos(path, nullptr, FileFormat::OpenImageIO);
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].width, static_cast<size_t>(kAnimatedGifWidth));
  EXPECT_EQ(infos[0].height, static_cast<size_t>(kAnimatedGifHeight));
  EXPECT_EQ(infos[0].depth, 1u);
  EXPECT_EQ(infos[0].numChannels, 4u);
  EXPECT_EQ(infos[0].numTimes, 2u);
  EXPECT_EQ(infos[0].bytesPerVoxel, 1u);
  EXPECT_EQ(infos[0].voxelFormat, VoxelFormat::Unsigned);
  EXPECT_TRUE(infos[0].lastChannelIsAlphaChannel);

  const ZImg img(path, ZImgRegion(), 0, 1, 1, 1, FileFormat::OpenImageIO);
  ASSERT_EQ(img.width(), static_cast<size_t>(kAnimatedGifWidth));
  ASSERT_EQ(img.height(), static_cast<size_t>(kAnimatedGifHeight));
  ASSERT_EQ(img.depth(), 1u);
  ASSERT_EQ(img.numChannels(), 4u);
  ASSERT_EQ(img.numTimes(), 2u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 0, 0), 255u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 1, 0), 0u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 2, 0), 0u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 3, 0), 255u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 0, 1), 0u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 1, 1), 255u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 2, 1), 0u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 3, 1), 255u);

  const ZImgRegion frameRegion(1, 3, 0, 2, 0, 1, 0, 4, 1, 2);
  const ZImg frame(path, frameRegion, 0, 1, 1, 1, FileFormat::OpenImageIO);
  ASSERT_EQ(frame.width(), 2u);
  ASSERT_EQ(frame.height(), 2u);
  ASSERT_EQ(frame.numChannels(), 4u);
  ASSERT_EQ(frame.numTimes(), 1u);
  EXPECT_EQ(*frame.data<uint8_t>(0, 0, 0, 0, 0), 0u);
  EXPECT_EQ(*frame.data<uint8_t>(0, 0, 0, 1, 0), 255u);
  EXPECT_EQ(*frame.data<uint8_t>(0, 0, 0, 2, 0), 0u);
  EXPECT_EQ(*frame.data<uint8_t>(0, 0, 0, 3, 0), 255u);

  EXPECT_THROW((ZImg(path, ZImgRegion(), 1, 1, 1, 1, FileFormat::OpenImageIO)), ZException);
}

TEST(ZImgOpenImageIO, ReadsDeltaGifAsCompositedFrames)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = QDir(tmp.path()).filePath(QStringLiteral("oiio_delta.gif"));
  writeDeltaGifFixture(path);
  ASSERT_TRUE(QFile::exists(path));

  const ZImgRegion secondFrameRegion(0, kAnimatedGifWidth, 0, kAnimatedGifHeight, 0, 1, 0, 4, 1, 2);
  const ZImg img(path, secondFrameRegion, 0, 1, 1, 1, FileFormat::OpenImageIO);
  ASSERT_EQ(img.width(), static_cast<size_t>(kAnimatedGifWidth));
  ASSERT_EQ(img.height(), static_cast<size_t>(kAnimatedGifHeight));
  ASSERT_EQ(img.numChannels(), 4u);
  ASSERT_EQ(img.numTimes(), 1u);

  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 0), 255u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 1), 0u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 2), 0u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 3), 255u);
  EXPECT_EQ(*img.data<uint8_t>(1, 0, 0, 0), 0u);
  EXPECT_EQ(*img.data<uint8_t>(1, 0, 0, 1), 255u);
  EXPECT_EQ(*img.data<uint8_t>(1, 0, 0, 2), 0u);
  EXPECT_EQ(*img.data<uint8_t>(1, 0, 0, 3), 255u);
  EXPECT_EQ(*img.data<uint8_t>(2, 1, 0, 0), 255u);
  EXPECT_EQ(*img.data<uint8_t>(2, 1, 0, 1), 0u);
  EXPECT_EQ(*img.data<uint8_t>(2, 1, 0, 2), 0u);
  EXPECT_EQ(*img.data<uint8_t>(2, 1, 0, 3), 255u);
}

TEST(ZImgOpenImageIO, ReadsTransparentGifAsCompositedFrames)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = QDir(tmp.path()).filePath(QStringLiteral("oiio_transparent.gif"));
  writeTransparentGifFixture(path);
  ASSERT_TRUE(QFile::exists(path));

  const ZImgRegion secondFrameRegion(0, kAnimatedGifWidth, 0, kAnimatedGifHeight, 0, 1, 0, 4, 1, 2);
  const ZImg img(path, secondFrameRegion, 0, 1, 1, 1, FileFormat::OpenImageIO);
  ASSERT_EQ(img.width(), static_cast<size_t>(kAnimatedGifWidth));
  ASSERT_EQ(img.height(), static_cast<size_t>(kAnimatedGifHeight));
  ASSERT_EQ(img.numChannels(), 4u);
  ASSERT_EQ(img.numTimes(), 1u);

  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 0), 255u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 1), 0u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 2), 0u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 3), 255u);
  EXPECT_EQ(*img.data<uint8_t>(1, 0, 0, 0), 0u);
  EXPECT_EQ(*img.data<uint8_t>(1, 0, 0, 1), 255u);
  EXPECT_EQ(*img.data<uint8_t>(1, 0, 0, 2), 0u);
  EXPECT_EQ(*img.data<uint8_t>(1, 0, 0, 3), 255u);
  EXPECT_EQ(*img.data<uint8_t>(2, 1, 0, 0), 255u);
  EXPECT_EQ(*img.data<uint8_t>(2, 1, 0, 1), 0u);
  EXPECT_EQ(*img.data<uint8_t>(2, 1, 0, 2), 0u);
  EXPECT_EQ(*img.data<uint8_t>(2, 1, 0, 3), 255u);
}

TEST(ZImgOpenImageIO, ReadsBackgroundDisposedGifAsCompositedFrames)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = QDir(tmp.path()).filePath(QStringLiteral("oiio_background_disposed.gif"));
  writeBackgroundDisposedGifFixture(path);
  ASSERT_TRUE(QFile::exists(path));

  const ZImgRegion secondFrameRegion(0, kAnimatedGifWidth, 0, kAnimatedGifHeight, 0, 1, 0, 4, 1, 2);
  const ZImg img(path, secondFrameRegion, 0, 1, 1, 1, FileFormat::OpenImageIO);
  ASSERT_EQ(img.width(), static_cast<size_t>(kAnimatedGifWidth));
  ASSERT_EQ(img.height(), static_cast<size_t>(kAnimatedGifHeight));
  ASSERT_EQ(img.numChannels(), 4u);
  ASSERT_EQ(img.numTimes(), 1u);

  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 0), 0u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 1), 0u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 2), 255u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 3), 255u);
  EXPECT_EQ(*img.data<uint8_t>(1, 0, 0, 0), 0u);
  EXPECT_EQ(*img.data<uint8_t>(1, 0, 0, 1), 255u);
  EXPECT_EQ(*img.data<uint8_t>(1, 0, 0, 2), 0u);
  EXPECT_EQ(*img.data<uint8_t>(1, 0, 0, 3), 255u);
  EXPECT_EQ(*img.data<uint8_t>(2, 1, 0, 0), 0u);
  EXPECT_EQ(*img.data<uint8_t>(2, 1, 0, 1), 0u);
  EXPECT_EQ(*img.data<uint8_t>(2, 1, 0, 2), 255u);
  EXPECT_EQ(*img.data<uint8_t>(2, 1, 0, 3), 255u);
}

TEST(ZImgOpenImageIO, RejectsInvalidScene)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = writeRgbPngFixture(tmp.path());
  EXPECT_THROW((ZImg(path, ZImgRegion(), 1, 1, 1, 1, FileFormat::OpenImageIO)), ZException);
}

TEST(ZImgOpenImageIO, ReadsPngFromMemory)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = writeRgbPngFixture(tmp.path());
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  QByteArray bytes = file.readAll();
  ASSERT_GT(bytes.size(), 0);

  ZImgInfo info;
  ZImgOpenImageIO::readMemInfo(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size(), info);
  ASSERT_EQ(info.width, 5u);
  ASSERT_EQ(info.height, 4u);
  ASSERT_EQ(info.numChannels, 3u);
  ASSERT_EQ(info.byteNumber(), 5u * 4u * 3u);

  std::vector<uint8_t> decoded(info.byteNumber());
  ZImgOpenImageIO::readMemImg(reinterpret_cast<const uint8_t*>(bytes.constData()),
                              bytes.size(),
                              decoded.data(),
                              decoded.size());
  ZImg img;
  img.wrapData(decoded.data(), info);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 0), 0u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 1), 50u);
  EXPECT_EQ(*img.data<uint8_t>(4, 3, 0, 2), 119u);
}

TEST(ZImgOpenImageIO, ReadsAnimatedGifFromMemory)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = QDir(tmp.path()).filePath(QStringLiteral("oiio_animated_memory.gif"));
  writeAnimatedGifFixture(path);
  ASSERT_TRUE(QFile::exists(path));

  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  QByteArray bytes = file.readAll();
  ASSERT_GT(bytes.size(), 0);

  ZImgInfo info;
  ZImgOpenImageIO::readMemInfo(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size(), info);
  ASSERT_EQ(info.width, static_cast<size_t>(kAnimatedGifWidth));
  ASSERT_EQ(info.height, static_cast<size_t>(kAnimatedGifHeight));
  ASSERT_EQ(info.numChannels, 4u);
  ASSERT_EQ(info.numTimes, 2u);
  ASSERT_EQ(info.byteNumber(), static_cast<size_t>(kAnimatedGifWidth * kAnimatedGifHeight * kAnimatedGifChannels * 2));

  std::vector<uint8_t> decoded(info.byteNumber());
  ZImgOpenImageIO::readMemImg(reinterpret_cast<const uint8_t*>(bytes.constData()),
                              bytes.size(),
                              decoded.data(),
                              decoded.size());
  ZImg img;
  img.wrapData(decoded.data(), info);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 0, 0), 255u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 1, 0), 0u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 0, 1), 0u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 1, 1), 255u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 3, 1), 255u);
}

TEST(ZImgOpenImageIO, ReadPriorityOrderFollowsFileFormatEnum)
{
  EXPECT_LT(FileFormat::Png, FileFormat::OpenImageIO);
  EXPECT_LT(FileFormat::OpenImageIO, FileFormat::BioFormats);
}

} // namespace
} // namespace nim
