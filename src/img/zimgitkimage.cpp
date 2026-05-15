#include "zimgitkimage.h"

#include "zimgsliceprovider.h"
#include "zstringutils.h"
#include <itkImageIOBase.h>
#include <itkNiftiImageIOFactory.h>
#include <itkNrrdImageIOFactory.h>
#include <itkImageIOFactory.h>
#include <itkMetaDataObject.h>
// #define ATLAS_SUPPORT_DICOM
#ifdef ATLAS_SUPPORT_DICOM
#include <itkGDCMImageIOFactory.h>
#endif

#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QDir>
#include <boost/regex.hpp>
#include <fstream>
#include <cctype>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <array>

namespace nim {

namespace {

void parseInfoFromImageIO(const itk::ImageIOBase* imageIO, ZImgInfo& info, bool isNd2);
void parseMetadataFromImageIO(const itk::ImageIOBase* imageIO, ZImgMetadata& meta);

struct NrrdRawSpec
{
  QString dataFilePath;
  uint64_t dataOffset{0};
  bool needByteSwap{false};
  uint32_t bytesPerVoxel{0};
  uint32_t numComponents{1};
  bool componentsFastest{true};
  uint64_t width{0};
  uint64_t height{0};
  uint64_t depth{0};
  uint64_t numTimes{1};
};

inline bool isLittleEndianHost()
{
  const uint16_t x = 1;
  return *reinterpret_cast<const uint8_t*>(&x) == 1;
}

inline std::string toLowerString(std::string s)
{
  for (auto& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

static bool
parseNrrdHeaderForRaw(const QString& filename, const itk::ImageIOBase* imageIO, const ZImgInfo& info, NrrdRawSpec& out)
{
  if (!(info.bytesPerVoxel == 1 || info.bytesPerVoxel == 2 || info.bytesPerVoxel == 4 || info.bytesPerVoxel == 8)) {
    return false;
  }

  std::ifstream in(QFile::encodeName(filename).constData(), std::ios::in | std::ios::binary);
  if (!in) {
    return false;
  }

  char magic[4];
  in.read(magic, 4);
  if (!in || std::strncmp(magic, "NRRD", 4) != 0) {
    return false;
  }
  std::string line;
  std::getline(in, line);

  bool hasEncoding = false;
  bool encodingIsRaw = false;
  bool hasDataFile = false;
  QString dataFilePath;
  int64_t byteSkip = 0;
  bool hasByteSkip = false;
  bool sawKinds = false;
  bool componentsFastest = false;

  std::streampos dataStartPos = std::streampos(0);
  while (std::getline(in, line)) {
    if (line.empty()) {
      dataStartPos = in.tellg();
      break;
    }
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty() && line[0] == '#') {
      continue;
    }
    auto colonPos = line.find(':');
    if (colonPos == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, colonPos);
    std::string value = line.substr(colonPos + 1);
    size_t i = 0;
    while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) {
      ++i;
    }
    value.erase(0, i);
    key = toLowerString(key);

    if (key == "encoding") {
      hasEncoding = true;
      auto v = toLowerString(value);
      encodingIsRaw = (v.find("raw") != std::string::npos);
    } else if (key == "data file") {
      hasDataFile = true;
      auto v = toLowerString(value);
      if (v.find("list") != std::string::npos) {
        return false;
      }
      if (!value.empty() && (value.front() == '"' || value.front() == '\'')) {
        value.erase(0, 1);
      }
      if (!value.empty() && (value.back() == '"' || value.back() == '\'')) {
        value.pop_back();
      }
      dataFilePath = QString::fromStdString(value);
    } else if (key == "byte skip") {
      try {
        long long v = std::stoll(value);
        byteSkip = v;
        hasByteSkip = true;
      }
      catch (...) {
        return false;
      }
    } else if (key == "kinds") {
      sawKinds = true;
      std::vector<std::string> tokens;
      std::string cur;
      std::istringstream iss(value);
      while (iss >> cur) {
        // tokens like RGB-color, RGBA-color, vector, complex, domain, time, space
        tokens.push_back(toLowerString(cur));
      }
      if (!tokens.empty()) {
        const std::string& t0 = tokens[0];
        auto isRange = [](const std::string& t) {
          return t.find("vector") != std::string::npos || t.find("color") != std::string::npos ||
                 t.find("complex") != std::string::npos || t.find("matrix") != std::string::npos ||
                 t.find("tensor") != std::string::npos;
        };
        componentsFastest = isRange(t0);
      }
    }
  }

  if (!hasEncoding || !encodingIsRaw) {
    return false;
  }

  if (hasDataFile) {
    QDir dir = QFileInfo(filename).dir();
    QString df = QDir::cleanPath(dir.filePath(dataFilePath));
    out.dataFilePath = df;
    if (!QFile::exists(out.dataFilePath)) {
      return false;
    }
    if (hasByteSkip && byteSkip < 0) {
      return false;
    }
    out.dataOffset = static_cast<uint64_t>(std::max<int64_t>(0, byteSkip));
  } else {
    if (dataStartPos <= 0) {
      return false;
    }
    out.dataFilePath = filename;
    uint64_t extraSkip = 0;
    if (hasByteSkip) {
      if (byteSkip == -1) {
        extraSkip = 0;
      } else if (byteSkip >= 0) {
        extraSkip = static_cast<uint64_t>(byteSkip);
      } else {
        return false;
      }
    }
    out.dataOffset = static_cast<uint64_t>(dataStartPos) + extraSkip;
  }

  out.width = info.width;
  out.height = info.height;
  out.depth = info.depth;
  out.numTimes = info.numTimes;
  out.bytesPerVoxel = static_cast<uint32_t>(info.bytesPerVoxel);
  out.numComponents = static_cast<uint32_t>(info.numChannels);

  if (out.numComponents > 1) {
    if (sawKinds) {
      if (!componentsFastest) {
        return false; // only support C as fastest axis for now
      }
    } else {
      // Without 'kinds', we cannot reliably infer axis order; be conservative
      return false;
    }
    out.componentsFastest = true;
  }

  bool hostLittle = isLittleEndianHost();
  bool fileLittle = true;
  switch (imageIO->GetByteOrder()) {
    case itk::IOByteOrderEnum::LittleEndian:
      fileLittle = true;
      break;
    case itk::IOByteOrderEnum::BigEndian:
      fileLittle = false;
      break;
    case itk::IOByteOrderEnum::OrderNotApplicable:
    default:
      fileLittle = hostLittle;
      break;
  }
  out.needByteSwap = (out.bytesPerVoxel > 1) && (hostLittle != fileLittle);

  return true;
}

// Parse sizes and kinds to compute file-dimension byte strides for X,Y,Z,C,T in file layout.
// Returns true if a consistent mapping is found. This enables handling of common axis orders
// beyond just C-fastest (e.g., XYCZT, XYZCT). We match spatial axes by size against
// width/height/depth from info; for channels/time, we use kinds or unique size matches.
static bool
computeNrrdDimensionStrides(const QString& filename, const ZImgInfo& info, std::array<size_t, 5>& dimStrides)
{
  std::ifstream in(QFile::encodeName(filename).constData(), std::ios::in | std::ios::binary);
  if (!in) {
    return false;
  }
  // Skip magic and first line
  char magic[4];
  in.read(magic, 4);
  if (!in || std::strncmp(magic, "NRRD", 4) != 0) {
    return false;
  }
  std::string line;
  std::getline(in, line);

  std::vector<uint64_t> sizes;
  std::vector<std::string> kinds;

  while (std::getline(in, line)) {
    if (line.empty()) {
      break;
    }
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty() && line[0] == '#') {
      continue;
    }
    auto colonPos = line.find(':');
    if (colonPos == std::string::npos) {
      continue;
    }
    std::string key = toLowerString(line.substr(0, colonPos));
    std::string value = line.substr(colonPos + 1);
    size_t i = 0;
    while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) {
      ++i;
    }
    value.erase(0, i);
    if (key == "sizes") {
      sizes.clear();
      std::istringstream iss(value);
      uint64_t v{};
      while (iss >> v) {
        sizes.push_back(v);
      }
    } else if (key == "kinds") {
      kinds.clear();
      std::istringstream iss(value);
      std::string tok;
      while (iss >> tok) {
        kinds.push_back(toLowerString(tok));
      }
    }
  }

  if (sizes.empty()) {
    return false;
  }
  const size_t nd = sizes.size();
  if (!kinds.empty() && kinds.size() != nd) {
    // inconsistent header
    return false;
  }

  auto isRange = [](const std::string& t) {
    return t.find("vector") != std::string::npos || t.find("color") != std::string::npos ||
           t.find("complex") != std::string::npos || t.find("matrix") != std::string::npos ||
           t.find("tensor") != std::string::npos;
  };

  int idxC = -1;
  int idxT = -1;
  std::vector<int> spatIdx;
  spatIdx.reserve(3);

  if (!kinds.empty()) {
    for (size_t iAxis = 0; iAxis < nd; ++iAxis) {
      const auto& k = kinds[iAxis];
      if (k.find("time") != std::string::npos) {
        idxT = static_cast<int>(iAxis);
      } else if (isRange(k)) {
        idxC = static_cast<int>(iAxis);
      } else {
        // treat others as spatial/domain axes
        spatIdx.push_back(static_cast<int>(iAxis));
      }
    }
  }

  // Deduce missing axes by size uniqueness when safe.
  if (idxC < 0 && info.numChannels > 1) {
    int found = -1;
    for (size_t iAxis = 0; iAxis < nd; ++iAxis) {
      if (sizes[iAxis] == info.numChannels) {
        if (found >= 0) {
          found = -2; // ambiguous
          break;
        }
        found = static_cast<int>(iAxis);
      }
    }
    if (found >= 0) {
      idxC = found;
    } else if (found == -2) {
      return false; // ambiguous
    }
  }
  if (idxT < 0 && info.numTimes > 1) {
    int found = -1;
    for (size_t iAxis = 0; iAxis < nd; ++iAxis) {
      if (sizes[iAxis] == info.numTimes) {
        if (found >= 0) {
          found = -2;
          break;
        }
        found = static_cast<int>(iAxis);
      }
    }
    if (found >= 0) {
      idxT = found;
    } else if (found == -2) {
      return false;
    }
  }

  // Build spatial axis candidates (exclude C/T)
  std::vector<int> spatialAxes;
  for (size_t iAxis = 0; iAxis < nd; ++iAxis) {
    if (static_cast<int>(iAxis) == idxC || static_cast<int>(iAxis) == idxT) {
      continue;
    }
    spatialAxes.push_back(static_cast<int>(iAxis));
  }
  // Map to X,Y,Z by matching sizes. Require exact match and uniqueness.
  auto pickAxisBySize = [&](uint64_t target) -> int {
    int chosen = -1;
    for (int ax : spatialAxes) {
      if (sizes[ax] == target) {
        if (chosen != -1) {
          return -2; // ambiguous
        }
        chosen = ax;
      }
    }
    // Remove chosen
    if (chosen >= 0) {
      spatialAxes.erase(std::remove(spatialAxes.begin(), spatialAxes.end(), chosen), spatialAxes.end());
    }
    return chosen;
  };

  int idxX = pickAxisBySize(info.width);
  if (idxX < 0) {
    return false;
  }
  int idxY = pickAxisBySize(info.height);
  if (idxY < 0) {
    return false;
  }
  int idxZ = -1;
  if (info.depth > 1) {
    idxZ = pickAxisBySize(info.depth);
    if (idxZ < 0) {
      return false;
    }
  } else {
    // if depth == 1, allow absence of Z axis (2D image)
    // pick remaining spatial if any size==1 as Z
    for (int ax : spatialAxes) {
      if (sizes[ax] == 1) {
        idxZ = ax;
        break;
      }
    }
    if (idxZ < 0) {
      // Keep a placeholder index even if not present; strides won't be used when depth==1.
      idxZ = 0;
    }
  }

  // Compute per-axis byte strides in file order
  std::vector<size_t> fileStride(nd, 0);
  size_t stride = static_cast<size_t>(info.bytesPerVoxel);
  for (size_t iAxis = 0; iAxis < nd; ++iAxis) {
    fileStride[iAxis] = stride;
    stride *= static_cast<size_t>(sizes[iAxis]);
  }

  // Map to XYZCT
  dimStrides[0] = fileStride[idxX];
  dimStrides[1] = fileStride[idxY];
  // For 2D images (depth == 1), assign the packed plane stride even if no explicit Z axis exists
  dimStrides[2] = (info.depth > 1) ? fileStride[idxZ] : static_cast<size_t>(info.planeByteNumber());
  // Channel stride: if no explicit component axis, use per-channel byte size (full volume)
  dimStrides[3] = (idxC >= 0) ? fileStride[idxC] : static_cast<size_t>(info.channelByteNumber());
  // Time stride: if absent, use bytes per time frame (full volume × channels)
  dimStrides[4] = (idxT >= 0) ? fileStride[idxT] : static_cast<size_t>(info.timeByteNumber());

  return true;
}

inline void bswapInPlace(void* data, size_t elemCount, size_t elemSize)
{
  auto* p = static_cast<uint8_t*>(data);
  switch (elemSize) {
    case 2:
      for (size_t i = 0; i < elemCount; ++i) {
        std::swap(p[0], p[1]);
        p += 2;
      }
      break;
    case 4:
      for (size_t i = 0; i < elemCount; ++i) {
        std::swap(p[0], p[3]);
        std::swap(p[1], p[2]);
        p += 4;
      }
      break;
    case 8:
      for (size_t i = 0; i < elemCount; ++i) {
        std::swap(p[0], p[7]);
        std::swap(p[1], p[6]);
        std::swap(p[2], p[5]);
        std::swap(p[3], p[4]);
        p += 8;
      }
      break;
    default:
      break; // 1-byte or unsupported sizes: no-op
  }
}

inline void byteSwapZImgInPlace(ZImg& img)
{
  const size_t elemSize = img.voxelByteNumber();
  if (elemSize <= 1) {
    return;
  }
  for (size_t t = 0; t < img.numTimes(); ++t) {
    for (size_t c = 0; c < img.numChannels(); ++c) {
      uint8_t* buf = img.channelData<uint8_t>(c, t);
      const size_t elemCount = img.channelVoxelNumber();
      bswapInPlace(buf, elemCount, elemSize);
    }
  }
}

} // namespace

ZImgITKImage::ZImgITKImage()
{
  try {
    if (itk::ObjectFactoryBase::GetRegisteredFactories().empty()) {
      itk::NiftiImageIOFactory::RegisterOneFactory();
      itk::NrrdImageIOFactory::RegisterOneFactory();
#ifdef ATLAS_SUPPORT_DICOM
      itk::GDCMImageIOFactory::RegisterOneFactory();
#endif
    }
  }
  catch (itk::ExceptionObject& err) {
    throw ZException(err.what(), ZException::Option::CheckErrno);
  }
}

QString ZImgITKImage::shortName() const
{
  return "ITKImage";
}

QString ZImgITKImage::fullName() const
{
  return "ITKImage";
}

QStringList ZImgITKImage::extensions() const
{
  QStringList res;

  try {
    for (const auto& pt : itk::ObjectFactoryBase::CreateAllInstance("itkImageIOBase")) {
      if (auto io = dynamic_cast<const itk::ImageIOBase*>(pt.GetPointer())) {
        // VLOG(1) << "ImageIO: " << io->GetNameOfClass();
        for (const auto& ext : io->GetSupportedReadExtensions()) {
          res.push_back(QString::fromStdString(ext));
          res.last().remove(0, 1); // remove '.'
        }
      }
    }
#ifdef ATLAS_SUPPORT_DICOM
    res.push_back("dcm");
#endif
  }
  catch (itk::ExceptionObject& err) {
    throw ZException(err.what(), ZException::Option::CheckErrno);
  }

  return res;
}

void ZImgITKImage::readInfo(const QString& filename,
                            std::vector<ZImgInfo>& infos,
                            std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  try {
    itk::ImageIOBase::Pointer imageIO =
      itk::ImageIOFactory::CreateImageIO(QFile::encodeName(filename).constData(),
                                         itk::ImageIOFactory::IOFileModeEnum::ReadMode);

    if (imageIO.IsNull()) {
      throw ZException("can not create reader", ZException::Option::CheckErrno);
    }

    imageIO->SetFileName(QFile::encodeName(filename).constData());
    imageIO->ReadImageInformation();

    bool isNrrd = QString(imageIO->GetNameOfClass()).contains("Nrrd");

    bool isNd2 = filename.endsWith(".nd2", Qt::CaseInsensitive);

    infos.resize(1);
    parseInfoFromImageIO(imageIO.GetPointer(), infos[0], isNd2);

    if (isNrrd) {
      // If this NRRD supports efficient raw region reads, advertise tiled subblocks
      NrrdRawSpec spec;
      if (parseNrrdHeaderForRaw(filename, imageIO.GetPointer(), infos[0], spec)) {
        std::array<size_t, 5> dimStrides{};
        if (computeNrrdDimensionStrides(filename, infos[0], dimStrides)) {
          createTiledSubBlocks(filename, infos, subBlocks);
        } else {
          createEmptySubBlocks(infos, subBlocks);
        }
      } else {
        // fall back: avoid per-slice tiles that would trigger repeated full-file reads
        createEmptySubBlocks(infos, subBlocks);
      }
    } else {
      createDefaultSubBlocks(filename, infos, subBlocks);
    }
  }
  catch (itk::ExceptionObject& err) {
    throw ZException(err.what(), ZException::Option::CheckErrno);
  }
}

void ZImgITKImage::readMetadata(const QString& filename, ZImgMetadata& meta, size_t /*scene*/)
{
  try {
    itk::ImageIOBase::Pointer imageIO =
      itk::ImageIOFactory::CreateImageIO(QFile::encodeName(filename).constData(),
                                         itk::ImageIOFactory::IOFileModeEnum::ReadMode);

    if (imageIO.IsNull()) {
      throw ZException("can not create reader", ZException::Option::CheckErrno);
    }

    imageIO->SetFileName(QFile::encodeName(filename).constData());
    imageIO->ReadImageInformation();

    parseMetadataFromImageIO(imageIO.GetPointer(), meta);
  }
  catch (itk::ExceptionObject& err) {
    throw ZException(err.what(), ZException::Option::CheckErrno);
  }
}

void ZImgITKImage::readThumbnail(const QString& /*filename*/,
                                 ZImgThumbernail& /*thumbnail*/,
                                 const ZImgRegion& /*region*/,
                                 size_t /*scene*/)
{
  try {
  }
  catch (itk::ExceptionObject& err) {
    throw ZException(err.what(), ZException::Option::CheckErrno);
  }
}

void ZImgITKImage::readImg(const QString& filename,
                           ZImg& img,
                           const ZImgRegion& region,
                           size_t scene,
                           const ZImgReadOptions& readOptions)
{
  if (scene != 0) {
    throw ZException("invalid scene");
  }

  try {
    itk::ImageIOBase::Pointer imageIO =
      itk::ImageIOFactory::CreateImageIO(QFile::encodeName(filename).constData(),
                                         itk::ImageIOFactory::IOFileModeEnum::ReadMode);

    if (imageIO.IsNull()) {
      throw ZException("can not create reader", ZException::Option::CheckErrno);
    }

    imageIO->SetFileName(QFile::encodeName(filename).constData());
    imageIO->ReadImageInformation();

    bool isNrrd = QString(imageIO->GetNameOfClass()).contains("Nrrd");

    bool isNd2 = filename.endsWith(".nd2", Qt::CaseInsensitive);

    ZImgInfo imgInfo;
    parseInfoFromImageIO(imageIO.GetPointer(), imgInfo, isNd2);

    if (region.isEmpty() || !region.isValid(imgInfo)) {
      throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", imgInfo, region));
    }

    if (isNd2) {
      ZImgRegion rgn = region;
      rgn.resolveRegionEnd(imgInfo);

      ZImgInfo clipInfo = rgn.clip(imgInfo);
      img = ZImg(clipInfo);

      itk::ImageIORegion ioRegion(5);
      ioRegion.SetIndex(0, rgn.start.x);
      ioRegion.SetIndex(1, rgn.start.y);
      ioRegion.SetIndex(2, rgn.start.z);
      ioRegion.SetIndex(3, rgn.start.t);
      ioRegion.SetIndex(4, rgn.start.c);
      ioRegion.SetSize(0, rgn.end.x - rgn.start.x);
      ioRegion.SetSize(1, rgn.end.y - rgn.start.y);
      ioRegion.SetSize(2, rgn.end.z - rgn.start.z);
      ioRegion.SetSize(3, rgn.end.t - rgn.start.t);
      ioRegion.SetSize(4, rgn.end.c - rgn.start.c);

      if (imageIO->GetNumberOfComponents() <= 1) {
        imageIO->SetIORegion(ioRegion);
        if (clipInfo.numTimes > 1) {
          auto buf = std::make_unique_for_overwrite<uint8_t[]>(img.byteNumber());
          imageIO->Read(buf.get());
          fixDimensionOrder(buf.get(), "XYZTC", img);
        } else {
          imageIO->Read(img.channelData(0));
        }
      } else {
        ZImgInfo scClipInfo = clipInfo;
        scClipInfo.numChannels = imageIO->GetNumberOfComponents();
        scClipInfo.createDefaultDescriptions();
        ZImg tmpScImg(scClipInfo);
        auto buf = std::make_unique_for_overwrite<uint8_t[]>(tmpScImg.byteNumber());
        // VLOG(2) << tmpScImg.byteNumber();
        for (auto ch = rgn.start.c; ch < rgn.end.c; ++ch) {
          ioRegion.SetIndex(4, ch);
          ioRegion.SetSize(4, 1);
          imageIO->SetIORegion(ioRegion);
          imageIO->Read(buf.get());
          fixDimensionOrder(buf.get(), "CXYZT", tmpScImg);
          size_t bestChannel = 0;
          double bestChannelSum = tmpScImg.createView(0, -1).sum();
          for (size_t c = 1; c < tmpScImg.numChannels(); ++c) {
            auto channelSum = tmpScImg.createView(c, -1).sum();
            if (channelSum > bestChannelSum) {
              bestChannel = c;
              bestChannelSum = channelSum;
            }
          }
          img.pasteImg(tmpScImg.createView(bestChannel, -1), ZVoxelCoordinate(0, 0, 0, ch, 0));
        }
      }
    } else {
      const bool wantsWhole = region.containsWholeImg(imgInfo);
      // Attempt NRRD fast-path only for partial requests
      ZImgRegion rgn = region;
      rgn.resolveRegionEnd(imgInfo);
      bool triedFastPath = false;
      if (isNrrd && !wantsWhole) {
        NrrdRawSpec spec;
        if (parseNrrdHeaderForRaw(filename, imageIO.GetPointer(), imgInfo, spec)) {
          std::array<size_t, 5> dimStrides{};
          if (computeNrrdDimensionStrides(filename, imgInfo, dimStrides)) {
            img = readRawImg(spec.dataFilePath, imgInfo, dimStrides, static_cast<size_t>(spec.dataOffset), rgn);
            if (spec.needByteSwap) {
              byteSwapZImgInPlace(img);
            }
            triedFastPath = true;
          }
        }
      }

      if (triedFastPath) {
        // Done
      } else if (wantsWhole || isNrrd) {
        // Full read then optional crop (for NRRD partial when fast-path unavailable)
        img = ZImg(imgInfo);
        itk::ImageIORegion ioRegion(4);
        ioRegion.SetIndex(0, 0);
        ioRegion.SetIndex(1, 0);
        ioRegion.SetIndex(2, 0);
        ioRegion.SetIndex(3, 0);
        ioRegion.SetSize(0, img.width());
        ioRegion.SetSize(1, img.height());
        ioRegion.SetSize(2, img.depth());
        ioRegion.SetSize(3, img.numTimes());
        imageIO->SetIORegion(ioRegion);
        if (imgInfo.numTimes > 1) {
          auto buf = std::make_unique_for_overwrite<uint8_t[]>(img.byteNumber());
          imageIO->Read(buf.get());
          fixDimensionOrder(buf.get(), "CXYZT", img);
        } else {
          imageIO->Read(img.channelData(0));
          if (imgInfo.numChannels > 1) {
            ZImg tpImg(imgInfo);
            CXYZtoXYZC(img, tpImg);
            img.swap(tpImg);
          }
        }
        if (!wantsWhole && isNrrd) {
          img = img.crop(region);
        }
      } else {
        rgn = region;
        rgn.resolveRegionEnd(imgInfo);

        bool clipChannel = rgn.start.c != 0 || rgn.end.c != int(imgInfo.numChannels);
        if (clipChannel) {
          rgn.start.c = 0;
          rgn.end.c = imgInfo.numChannels;
        }

        ZImgInfo clipInfo = rgn.clip(imgInfo);
        ZImg tmpImg(clipInfo);

        itk::ImageIORegion ioRegion(4);
        ioRegion.SetIndex(0, rgn.start.x);
        ioRegion.SetIndex(1, rgn.start.y);
        ioRegion.SetIndex(2, rgn.start.z);
        ioRegion.SetIndex(3, rgn.start.t);
        ioRegion.SetSize(0, rgn.end.x - rgn.start.x);
        ioRegion.SetSize(1, rgn.end.y - rgn.start.y);
        ioRegion.SetSize(2, rgn.end.z - rgn.start.z);
        ioRegion.SetSize(3, rgn.end.t - rgn.start.t);
        imageIO->SetIORegion(ioRegion);
        if (clipInfo.numTimes > 1) {
          auto buf = std::make_unique_for_overwrite<uint8_t[]>(tmpImg.byteNumber());
          imageIO->Read(buf.get());
          fixDimensionOrder(buf.get(), "CXYZT", tmpImg);
        } else {
          imageIO->Read(tmpImg.channelData(0));
          if (clipInfo.numChannels > 1) {
            ZImg tpImg(clipInfo);
            CXYZtoXYZC(tmpImg, tpImg);
            tmpImg.swap(tpImg);
          }
        }

        if (clipChannel) {
          ZImgRegion crgn;
          crgn.start.c = region.start.c;
          crgn.end.c = region.end.c;
          img = tmpImg.crop(crgn);
        } else {
          img.swap(tmpImg);
        }
      }
    }

    if (readOptions.includeMetadata) {
      parseMetadataFromImageIO(imageIO.GetPointer(), img.metadataRef());
    }
  }
  catch (itk::ExceptionObject& err) {
    throw ZException(err.what(), ZException::Option::CheckErrno);
  }
}

void ZImgITKImage::checkImgBeforeWriting(const QString& filename,
                                         const ZImgInfo& info,
                                         const ZImgWriteParameters& paras)
{
  ZImgFormat::checkImgBeforeWriting(filename, info, paras);
  if (!filename.endsWith(".nrrd", Qt::CaseInsensitive)) {
    throw ZException("only support nrrd format for now");
  }
  if (!(paras.compression == Compression::AUTO || paras.compression == Compression::NONE ||
        paras.compression == Compression::DEFLATE)) {
    throw ZException(fmt::format("compression {} is not supported", paras.compression));
  }
  if (info.numTimes != 1) {
    throw ZException("time sequence image is not supported");
  }
}

void ZImgITKImage::writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, img.info(), paras);

  //
}

bool ZImgITKImage::supportRead() const
{
  return true;
}

bool ZImgITKImage::supportWrite() const
{
  return false;
}

namespace {

void parseInfoFromImageIO(const itk::ImageIOBase* imageIO, ZImgInfo& info, bool isNd2)
{
  auto ndims = imageIO->GetNumberOfDimensions();
  if (ndims == 1) {
    info.width = imageIO->GetDimensions(0);
    info.height = 1;
    info.depth = 1;
    info.numTimes = 1;
  } else if (ndims == 2) {
    info.width = imageIO->GetDimensions(0);
    info.height = imageIO->GetDimensions(1);
    info.depth = 1;
    info.numTimes = 1;
  } else if (ndims == 3) {
    info.width = imageIO->GetDimensions(0);
    info.height = imageIO->GetDimensions(1);
    info.depth = imageIO->GetDimensions(2);
    info.numTimes = 1;
    if (isNd2) {
      info.numChannels = 1;
    }
  } else if (ndims == 4) {
    info.width = imageIO->GetDimensions(0);
    info.height = imageIO->GetDimensions(1);
    info.depth = imageIO->GetDimensions(2);
    info.numTimes = imageIO->GetDimensions(3);
    if (isNd2) {
      info.numChannels = 1;
    }
  } else if (ndims == 5 && isNd2) {
    info.width = imageIO->GetDimensions(0);
    info.height = imageIO->GetDimensions(1);
    info.depth = imageIO->GetDimensions(2);
    info.numTimes = imageIO->GetDimensions(3);
    info.numChannels = imageIO->GetDimensions(4);
  } else {
    throw ZException(fmt::format("NDims not supported: {}", ndims));
  }
  if (!isNd2) {
    info.numChannels = imageIO->GetNumberOfComponents();
  }
  switch (imageIO->GetComponentType()) {
    case itk::IOComponentEnum::CHAR:
      info.bytesPerVoxel = 1;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case itk::IOComponentEnum::UCHAR:
      info.bytesPerVoxel = 1;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case itk::IOComponentEnum::SHORT:
      info.bytesPerVoxel = 2;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case itk::IOComponentEnum::USHORT:
      info.bytesPerVoxel = 2;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case itk::IOComponentEnum::INT:
      info.bytesPerVoxel = 4;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case itk::IOComponentEnum::UINT:
      info.bytesPerVoxel = 4;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case itk::IOComponentEnum::LONG:
      info.bytesPerVoxel = 8;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case itk::IOComponentEnum::ULONG:
      info.bytesPerVoxel = 8;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case itk::IOComponentEnum::FLOAT:
      info.bytesPerVoxel = 4;
      info.voxelFormat = VoxelFormat::Float;
      break;
    case itk::IOComponentEnum::DOUBLE:
      info.bytesPerVoxel = 8;
      info.voxelFormat = VoxelFormat::Float;
      break;
    default:
      throw ZException("Not supported ElementType");
  }

  info.voxelSizeUnit = VoxelSizeUnit::mm;
  if (ndims == 1) {
    info.voxelSizeX = imageIO->GetSpacing(0);
  } else if (ndims == 2) {
    info.voxelSizeX = imageIO->GetSpacing(0);
    info.voxelSizeY = imageIO->GetSpacing(1);
  } else if (ndims >= 3) {
    info.voxelSizeX = imageIO->GetSpacing(0);
    info.voxelSizeY = imageIO->GetSpacing(1);
    info.voxelSizeZ = imageIO->GetSpacing(2);
  }
  info.createDefaultDescriptions();
  if (ndims == 4) {
    for (auto& timeStamp : info.timeStamps) {
      timeStamp *= imageIO->GetSpacing(3);
    }
  }

  if (info.isEmpty()) {
    throw ZException("Empty Image");
  }

  if (isNd2) {
    using DictionaryType = itk::MetaDataDictionary;
    const DictionaryType& dictionary = imageIO->GetMetaDataDictionary();

    using MetaDataStringType = itk::MetaDataObject<std::string>;

    std::vector<size_t> usedChannels;
    std::string key = "sSpecSettings";
    if (dictionary.HasKey(key)) {
      if (auto value = dynamic_cast<const MetaDataStringType*>(dictionary.Get(key)); value) {
        static const boost::regex channelInfo(R"(^CH(\d+)\s+{Laser Wavelength}:.*)");
        std::vector<absl::string_view> lines = absl::StrSplit(value->GetMetaDataObjectValue(), '\n');

        for (auto line : lines) {
          boost::match_results<std::string_view::iterator> match;
          if (boost::regex_match(line.begin(), line.end(), match, channelInfo)) {
            size_t channelNum;
            if (stringToValueNoThrow(match[1].first, match[1].second, channelNum)) {
              usedChannels.push_back(channelNum);
              VLOG(1) << line << " " << usedChannels.back();
            } else {
              LOG(ERROR) << "Failed to convert channel number: " << line;
            }
          }
        }
      }
    }
    if (usedChannels.size() < info.numChannels) {
      usedChannels.clear();
    }

    for (size_t ch = 0; ch < info.numChannels; ++ch) {
      auto actualChannel = usedChannels.empty() ? ch + 1 : usedChannels[ch];
      key = fmt::format("CH{}ChannelColor", actualChannel);
      if (dictionary.HasKey(key)) {
        if (auto value = dynamic_cast<const MetaDataStringType*>(dictionary.Get(key)); value) {
          int32_t color;
          if (!stringToValueNoThrow(value->GetMetaDataObjectValue(), color)) {
            throw ZException(fmt::format("parse nd2 channel color {} error", value->GetMetaDataObjectValue()));
          }
          col4 col = std::bit_cast<col4>(color);
          col.a = 255;
          info.channelColors[ch] = col;
        }
      }
      key = fmt::format("CH{}ChannelDyeName", actualChannel);
      if (dictionary.HasKey(key)) {
        if (auto value = dynamic_cast<const MetaDataStringType*>(dictionary.Get(key)); value) {
          info.channelNames[ch] = value->GetMetaDataObjectValue();
        }
      }
    }

    VLOG(2) << info;
  }
}

void parseMetadataFromImageIO(const itk::ImageIOBase* imageIO, ZImgMetadata& meta)
{
  using DictionaryType = itk::MetaDataDictionary;
  const DictionaryType& dictionary = imageIO->GetMetaDataDictionary();

  using MetaDataStringType = itk::MetaDataObject<std::string>;

  auto itr = dictionary.Begin();
  auto end = dictionary.End();

  while (itr != end) {
    itk::MetaDataObjectBase::Pointer entry = itr->second;
    MetaDataStringType::Pointer entryvalue = dynamic_cast<MetaDataStringType*>(entry.GetPointer());
    if (entryvalue) {
      std::string tagkey = itr->first;
      std::string tagvalue = entryvalue->GetMetaDataObjectValue();
      // std::cout << tagkey << " = " << tagvalue << std::endl;
      meta.attachToTopLevel(ZImgMetatag(tagkey, tagvalue));
    }
    ++itr;
  }
}

} // namespace

} // namespace nim
