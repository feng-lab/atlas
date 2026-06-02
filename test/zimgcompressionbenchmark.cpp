#include "zh5zjpegxr.h"
#include "zh5zzstd.h"
#include "zimg.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <zstd.h>

namespace {

using namespace nim;

struct CodecRun
{
  std::string label;
  ZImgWriteParameters parameters;
};

std::string usage()
{
  return "usage: zimgcompressionbenchmark --input <file.nim> --output-dir <dir> "
         "[--codecs zip,zstd,zstd3,jxr,jxr1,...] [--keep-output] [--info-only]\n"
         "  codec names: zip, zstd or zstd-7..zstdN, jxr or jxr0.0..jxr1.0\n";
}

bool consumeValue(int& index, int argc, char* argv[], std::string_view flag, std::string& value)
{
  if (argv[index] != flag) {
    return false;
  }
  if (index + 1 >= argc) {
    throw ZException(fmt::format("missing value for {}", flag));
  }
  value = argv[index + 1];
  ++index;
  return true;
}

std::vector<std::string> splitCommaList(std::string_view value)
{
  std::vector<std::string> result;
  size_t begin = 0;
  while (begin <= value.size()) {
    const size_t end = value.find(',', begin);
    std::string item(value.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
    if (!item.empty()) {
      result.push_back(std::move(item));
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return result;
}

CodecRun makeCodecRun(const std::string& name)
{
  CodecRun run;
  run.label = name;
  if (name == "zip") {
    run.parameters.compression = Compression::DEFLATE;
    run.parameters.zlibCompressionLevel = 6;
  } else if (name == "zstd") {
    run.parameters.compression = Compression::ZSTD;
  } else if (name.starts_with("zstd")) {
    const std::string levelString = name.substr(4);
    char* end = nullptr;
    const long level = std::strtol(levelString.c_str(), &end, 10);
    if (end == levelString.c_str() || *end != '\0' || level < ZSTD_minCLevel() || level > ZSTD_maxCLevel()) {
      throw ZException(fmt::format("invalid Zstd level codec name: {}", name));
    }
    run.parameters.compression = Compression::ZSTD;
    run.parameters.zstdCompressionLevel = static_cast<int32_t>(level);
  } else if (name == "jxr") {
    run.parameters.compression = Compression::JPEGXR;
  } else if (name.starts_with("jxr")) {
    const std::string qualityString = name.substr(3);
    char* end = nullptr;
    const double quality = std::strtod(qualityString.c_str(), &end);
    if (end == qualityString.c_str() || *end != '\0' || quality < 0.0 || quality > 1.0) {
      throw ZException(fmt::format("invalid JPEG XR quality codec name: {}", name));
    }
    run.parameters.compression = Compression::JPEGXR;
    run.parameters.jpegXRQuality = quality;
  } else {
    throw ZException(fmt::format("unknown codec: {}", name));
  }
  return run;
}

QString outputFilename(const QString& outputDir, const QString& input, const std::string& label)
{
  QFileInfo inputInfo(input);
  return QDir(outputDir).filePath(inputInfo.completeBaseName() + "_" + QString::fromStdString(label) + ".nim");
}

double secondsSince(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main(int argc, char* argv[])
{
  try {
    QString input;
    QString outputDir = QDir::currentPath();
    std::string codecs = "zip,zstd,zstd3,zstd5,zstd9,jxr";
    bool keepOutput = false;
    bool infoOnly = false;

    for (int i = 1; i < argc; ++i) {
      std::string value;
      if (consumeValue(i, argc, argv, "--input", value)) {
        input = QString::fromStdString(value);
      } else if (consumeValue(i, argc, argv, "--output-dir", value)) {
        outputDir = QString::fromStdString(value);
      } else if (consumeValue(i, argc, argv, "--codecs", codecs)) {
      } else if (std::string_view(argv[i]) == "--keep-output") {
        keepOutput = true;
      } else if (std::string_view(argv[i]) == "--info-only") {
        infoOnly = true;
      } else if (std::string_view(argv[i]) == "--help") {
        std::cout << usage();
        return 0;
      } else {
        throw ZException(fmt::format("unknown argument: {}", argv[i]));
      }
    }

    if (input.isEmpty()) {
      throw ZException("missing --input");
    }
    QDir dir(outputDir);
    if (!dir.exists() && !dir.mkpath(".")) {
      throw ZException(fmt::format("failed to create output dir {}", outputDir.toStdString()));
    }

    jpegxr_register_h5filter();
    zstd_register_h5filter();

    if (infoOnly) {
      const std::vector<ZImgInfo> infos = ZImg::readImgInfos(input, nullptr, FileFormat::HDF5Img);
      if (infos.size() != 1) {
        throw ZException(fmt::format("expected one scene in {}, got {}", input.toStdString(), infos.size()));
      }
      const ZImgInfo info = infos.front();
      const QFileInfo inputFile(input);
      std::cout << "input,path,size_bytes,decoded_bytes,width,height,depth,channels,times,type\n";
      std::cout << "input," << input.toStdString() << "," << inputFile.size() << "," << info.byteNumber() << ","
                << info.width << "," << info.height << "," << info.depth << "," << info.numChannels << ","
                << info.numTimes << "," << info.typeAsString() << "\n";
      return 0;
    }

    const auto loadStart = std::chrono::steady_clock::now();
    ZImg source(input, ZImgRegion(), 0, 1, 1, 1, FileFormat::HDF5Img);
    const double loadSeconds = secondsSince(loadStart);
    const ZImgInfo info = source.info();
    const QFileInfo inputFile(input);
    std::cout << "input,path,size_bytes,decoded_bytes,load_seconds,width,height,depth,channels,times,type\n";
    std::cout << "input," << input.toStdString() << "," << inputFile.size() << "," << info.byteNumber() << ","
              << loadSeconds << "," << info.width << "," << info.height << "," << info.depth << "," << info.numChannels
              << "," << info.numTimes << "," << info.typeAsString() << "\n";
    std::cout << "codec,size_bytes,seconds,MiB_per_s,kept,path\n";

    for (const std::string& codecName : splitCommaList(codecs)) {
      CodecRun run = makeCodecRun(codecName);
      const QString out = outputFilename(outputDir, input, run.label);
      QFile::remove(out);

      const auto start = std::chrono::steady_clock::now();
      source.save(out, FileFormat::HDF5Img, run.parameters);
      const double seconds = secondsSince(start);

      const QFileInfo outInfo(out);
      if (!outInfo.exists()) {
        throw ZException(fmt::format("output file was not created: {}", out.toStdString()));
      }
      const double mibPerSecond =
        seconds > 0 ? (static_cast<double>(info.byteNumber()) / 1024.0 / 1024.0) / seconds : 0.0;
      std::cout << run.label << "," << outInfo.size() << "," << seconds << "," << mibPerSecond << ","
                << (keepOutput ? "true" : "false") << "," << out.toStdString() << "\n";
      std::cout.flush();

      if (!keepOutput) {
        QFile::remove(out);
      }
    }
  }
  catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    std::cerr << usage();
    return 1;
  }
  return 0;
}
