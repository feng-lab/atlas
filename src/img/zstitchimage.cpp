#include "zstitchimage.h"

#include "zlog.h"
#include "zimgnccmatch.h"
#include "zimgregion.h"
#include "zstringutils.h"
#include "zeigenutils.h"
#include "zvbgmm.h"
#include <QFileInfo>
#include <QDir>
#include <folly/ScopeGuard.h>
#include <tbb/parallel_for.h>
#include <tbb/global_control.h>
#include <boost/unordered/concurrent_flat_map.hpp>
#include <algorithm>
#include <limits>

DECLARE_uint32(zimg_global_fft_number_of_threads);

namespace {

using namespace nim;

void buildFullConnection(size_t nStacks, std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint>& conn)
{
  if (nStacks <= 1) {
    return;
  }

  for (size_t f = 0; f < nStacks; ++f) {
    for (size_t m = f + 1; m < nStacks; ++m) {
      conn[std::make_pair(f, m)] = ZImgNCCMatch::PositionHint::None;
    }
  }
}

size_t buildConnectionFromTextFile(const QString& filename,
                                   std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint>& conn)
{
  size_t nStacks = 0;

  constexpr std::array headers = {"# img1"sv, "img2"sv, "img2 position"sv};

  std::ifstream file = openIFStream(filename, std::ios_base::in);
  if (!file) {
    throw ZException("Can not open file", ZException::Option::CheckErrno);
  }

  std::string line;
  while (std::getline(file, line)) {
    auto cleanLineView = absl::StripAsciiWhitespace(removeComment(line));
    std::vector<std::string_view> fieldList =
      absl::StrSplit(cleanLineView, absl::ByAnyChar(delimiter_literal), absl::SkipEmpty());
    if (fieldList.size() >= 3) {
      int32_t idx1, idx2;
      stringToValue(fieldList[0], idx1);
      stringToValue(fieldList[1], idx2);
      if (idx1 <= 0 || idx2 <= 0) {
        throw ZException(fmt::format("Can not parse line ({}) with format <{}>", line, fmt::join(headers, ", ")));
      }
      auto stackPair = std::make_pair<size_t, size_t>(idx1 - 1, idx2 - 1);
      nStacks = std::max<size_t>(nStacks, idx1);
      nStacks = std::max<size_t>(nStacks, idx2);
      conn[stackPair] = ZImgNCCMatch::PositionHint::None;
      for (size_t i = 2; i < fieldList.size(); ++i) {
        if (absl::EqualsIgnoreCase(fieldList[i], "Down"sv)) {
          conn[stackPair] = conn[stackPair] | ZImgNCCMatch::PositionHint::Down;
        } else if (absl::EqualsIgnoreCase(fieldList[i], "Right"sv)) {
          conn[stackPair] = conn[stackPair] | ZImgNCCMatch::PositionHint::Right;
        } else if (absl::EqualsIgnoreCase(fieldList[i], "Back"sv)) {
          conn[stackPair] = conn[stackPair] | ZImgNCCMatch::PositionHint::Back;
        } else {
          throw ZException(fmt::format("Can not parse line ({}) with format <{}>", line, fmt::join(headers, ", ")));
        }
      }
    } else if (!cleanLineView.empty()) {
      throw ZException(fmt::format("Wrong connection text format: {}", line));
    }
  }

  if (!file.eof()) {
    throw ZException(fmt::format("Error while reading file {}", filename), ZException::Option::CheckErrno);
  }

  return nStacks;
}

size_t buildConnectionFromGrid(const ZImg& grid,
                               std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint>& connRes)
{
  size_t nStacks = 0;

  std::map<size_t, int> idxToMinConnDist;
  CHECK(grid.info().isType<int32_t>());
  for (size_t z = 0; z < grid.depth(); ++z) {
    for (size_t y = 0; y < grid.height(); ++y) {
      for (size_t x = 0; x < grid.width(); ++x) {
        if (grid.value<int32_t>(x, y, z) > 0) {
          size_t idx1 = grid.value<int32_t>(x, y, z) - 1;
          idxToMinConnDist[idx1] = 10;
        }
      }
    }
  }

  std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint> conn;
  std::map<std::pair<size_t, size_t>, int> connDist;
  for (size_t z = 0; z < grid.depth(); ++z) {
    for (size_t y = 0; y < grid.height(); ++y) {
      for (size_t x = 0; x < grid.width(); ++x) {
        if (grid.value<int32_t>(x, y, z) > 0) {
          size_t idx1 = grid.value<int32_t>(x, y, z) - 1;

          if (x + 1 < grid.width() && grid.value<int32_t>(x + 1, y, z) > 0) { // right
            size_t idx2 = grid.value<int32_t>(x + 1, y, z) - 1;
            std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
            conn[stackPair] = ZImgNCCMatch::PositionHint::Right;
            idxToMinConnDist[idx1] = std::min(1, idxToMinConnDist[idx1]);
            idxToMinConnDist[idx2] = std::min(1, idxToMinConnDist[idx2]);
            connDist[stackPair] = 1;
          }
          if (y + 1 < grid.height() && grid.value<int32_t>(x, y + 1, z) > 0) { // down
            size_t idx2 = grid.value<int32_t>(x, y + 1, z) - 1;
            std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
            conn[stackPair] = ZImgNCCMatch::PositionHint::Down;
            idxToMinConnDist[idx1] = std::min(1, idxToMinConnDist[idx1]);
            idxToMinConnDist[idx2] = std::min(1, idxToMinConnDist[idx2]);
            connDist[stackPair] = 1;
          }
          if (z + 1 < grid.depth() && grid.value<int32_t>(x, y, z + 1) > 0) { // back
            size_t idx2 = grid.value<int32_t>(x, y, z + 1) - 1;
            std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
            conn[stackPair] = ZImgNCCMatch::PositionHint::Back;
            idxToMinConnDist[idx1] = std::min(1, idxToMinConnDist[idx1]);
            idxToMinConnDist[idx2] = std::min(1, idxToMinConnDist[idx2]);
            connDist[stackPair] = 1;
          }

          if (x + 1 < grid.width() && y + 1 < grid.height() && grid.value<int32_t>(x + 1, y + 1, z) > 0) { // right down
            size_t idx2 = grid.value<int32_t>(x + 1, y + 1, z) - 1;
            std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
            conn[stackPair] = ZImgNCCMatch::PositionHint::Right | ZImgNCCMatch::PositionHint::Down;
            idxToMinConnDist[idx1] = std::min(2, idxToMinConnDist[idx1]);
            idxToMinConnDist[idx2] = std::min(2, idxToMinConnDist[idx2]);
            connDist[stackPair] = 2;
          }
          if (x > 0 && y + 1 < grid.height() && grid.value<int32_t>(x - 1, y + 1, z) > 0) { // left down
            size_t idx2 = grid.value<int32_t>(x - 1, y + 1, z) - 1;
            std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
            conn[stackPair] = ZImgNCCMatch::PositionHint::Left | ZImgNCCMatch::PositionHint::Down;
            idxToMinConnDist[idx1] = std::min(2, idxToMinConnDist[idx1]);
            idxToMinConnDist[idx2] = std::min(2, idxToMinConnDist[idx2]);
            connDist[stackPair] = 2;
          }
          if (x > 0 && z + 1 < grid.depth() && grid.value<int32_t>(x - 1, y, z + 1) > 0) { // left back
            size_t idx2 = grid.value<int32_t>(x - 1, y, z + 1) - 1;
            std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
            conn[stackPair] = ZImgNCCMatch::PositionHint::Left | ZImgNCCMatch::PositionHint::Back;
            idxToMinConnDist[idx1] = std::min(2, idxToMinConnDist[idx1]);
            idxToMinConnDist[idx2] = std::min(2, idxToMinConnDist[idx2]);
            connDist[stackPair] = 2;
          }
          if (y > 0 && z + 1 < grid.depth() && grid.value<int32_t>(x, y - 1, z + 1) > 0) { // up back
            size_t idx2 = grid.value<int32_t>(x, y - 1, z + 1) - 1;
            std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
            conn[stackPair] = ZImgNCCMatch::PositionHint::Up | ZImgNCCMatch::PositionHint::Back;
            idxToMinConnDist[idx1] = std::min(2, idxToMinConnDist[idx1]);
            idxToMinConnDist[idx2] = std::min(2, idxToMinConnDist[idx2]);
            connDist[stackPair] = 2;
          }
          if (x + 1 < grid.width() && z + 1 < grid.depth() && grid.value<int32_t>(x + 1, y, z + 1) > 0) { // right back
            size_t idx2 = grid.value<int32_t>(x + 1, y, z + 1) - 1;
            std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
            conn[stackPair] = ZImgNCCMatch::PositionHint::Right | ZImgNCCMatch::PositionHint::Back;
            idxToMinConnDist[idx1] = std::min(2, idxToMinConnDist[idx1]);
            idxToMinConnDist[idx2] = std::min(2, idxToMinConnDist[idx2]);
            connDist[stackPair] = 2;
          }
          if (y + 1 < grid.height() && z + 1 < grid.depth() && grid.value<int32_t>(x, y + 1, z + 1) > 0) { // down back
            size_t idx2 = grid.value<int32_t>(x, y + 1, z + 1) - 1;
            std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
            conn[stackPair] = ZImgNCCMatch::PositionHint::Down | ZImgNCCMatch::PositionHint::Back;
            idxToMinConnDist[idx1] = std::min(2, idxToMinConnDist[idx1]);
            idxToMinConnDist[idx2] = std::min(2, idxToMinConnDist[idx2]);
            connDist[stackPair] = 2;
          }

          if (z + 1 < grid.depth()) {
            if (x + 1 < grid.width() && y + 1 < grid.height() &&
                grid.value<int32_t>(x + 1, y + 1, z + 1) > 0) { // right down back
              size_t idx2 = grid.value<int32_t>(x + 1, y + 1, z + 1) - 1;
              std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
              conn[stackPair] =
                ZImgNCCMatch::PositionHint::Right | ZImgNCCMatch::PositionHint::Down | ZImgNCCMatch::PositionHint::Back;
              idxToMinConnDist[idx1] = std::min(3, idxToMinConnDist[idx1]);
              idxToMinConnDist[idx2] = std::min(3, idxToMinConnDist[idx2]);
              connDist[stackPair] = 3;
            }
            if (x > 0 && y + 1 < grid.height() && grid.value<int32_t>(x - 1, y + 1, z + 1) > 0) { // left down back
              size_t idx2 = grid.value<int32_t>(x - 1, y + 1, z + 1) - 1;
              std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
              conn[stackPair] =
                ZImgNCCMatch::PositionHint::Left | ZImgNCCMatch::PositionHint::Down | ZImgNCCMatch::PositionHint::Back;
              idxToMinConnDist[idx1] = std::min(3, idxToMinConnDist[idx1]);
              idxToMinConnDist[idx2] = std::min(3, idxToMinConnDist[idx2]);
              connDist[stackPair] = 3;
            }
            if (x + 1 < grid.width() && y > 0 && grid.value<int32_t>(x + 1, y - 1, z + 1) > 0) { // right up back
              size_t idx2 = grid.value<int32_t>(x + 1, y - 1, z + 1) - 1;
              std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
              conn[stackPair] =
                ZImgNCCMatch::PositionHint::Right | ZImgNCCMatch::PositionHint::Up | ZImgNCCMatch::PositionHint::Back;
              idxToMinConnDist[idx1] = std::min(3, idxToMinConnDist[idx1]);
              idxToMinConnDist[idx2] = std::min(3, idxToMinConnDist[idx2]);
              connDist[stackPair] = 3;
            }
            if (x > 0 && y > 0 && grid.value<int32_t>(x - 1, y - 1, z + 1) > 0) { // left up back
              size_t idx2 = grid.value<int32_t>(x - 1, y - 1, z + 1) - 1;
              std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
              conn[stackPair] =
                ZImgNCCMatch::PositionHint::Left | ZImgNCCMatch::PositionHint::Up | ZImgNCCMatch::PositionHint::Back;
              idxToMinConnDist[idx1] = std::min(3, idxToMinConnDist[idx1]);
              idxToMinConnDist[idx2] = std::min(3, idxToMinConnDist[idx2]);
              connDist[stackPair] = 3;
            }
          }
        }
      }
    }
  }

  for (const auto& [key, val] : idxToMinConnDist) {
    if (val > 3) {
      throw ZException(QString("Can not stitch because images are not connected. Abort."));
    }
  }

  // prune
  for (const auto& [key, val] : conn) {
    if (connDist[key] == idxToMinConnDist[key.first] || connDist[key] == idxToMinConnDist[key.second]) {
      connRes[key] = val;
      nStacks = std::max(nStacks, key.first + 1);
      nStacks = std::max(nStacks, key.second + 1);
    }
  }

  return nStacks;
}

void buildConnectionFromRegions(const std::vector<ZImgRegion>& rgns,
                                std::map<std::pair<size_t, size_t>, ZVoxelCoordinate>& connRes)
{
  std::map<std::pair<size_t, size_t>, std::pair<ZVoxelCoordinate, double>> conn;
  std::map<size_t, std::vector<std::pair<size_t, double>>> idxToConn;
  std::vector<double> allOverlaps;
  Eigen::MatrixXd allOverlapMat;
  for (size_t f = 0; f < rgns.size(); ++f) {
    const ZImgRegion& rgnf = rgns[f];
    double areaf = (rgnf.end.y - rgnf.start.y) * 1.0 * (rgnf.end.x - rgnf.start.x);
    for (size_t m = f + 1; m < rgns.size(); ++m) {
      const ZImgRegion& rgnm = rgns[m];
      if (rgnm.start.x >= rgnf.end.x || rgnm.end.x <= rgnf.start.x || rgnm.start.y >= rgnf.end.y ||
          rgnm.end.y <= rgnf.start.y) {
        continue; // no overlap
      }
      double aream = (rgnm.end.y - rgnm.start.y) * 1.0 * (rgnm.end.x - rgnm.start.x);
      double overlaparea = (std::min(rgnm.end.y, rgnf.end.y) - std::max(rgnm.start.y, rgnf.start.y)) * 1.0 *
                           (std::min(rgnm.end.x, rgnf.end.x) - std::max(rgnm.start.x, rgnf.start.x));
      double overlap = std::max(overlaparea / areaf, overlaparea / aream);
      allOverlaps.push_back(overlap);
      conn[std::make_pair(f, m)] = std::make_pair(rgnm.start - rgnf.start, overlap);
      idxToConn[f].emplace_back(m, overlap);
      idxToConn[m].emplace_back(f, overlap);
    }
  }

  allOverlapMat.resize(allOverlaps.size(), 1);
  for (size_t r = 0; r < allOverlaps.size(); ++r) {
    allOverlapMat(r, 0) = allOverlaps[r];
  }
  ZVBGMM<double, double> vbgmm(allOverlapMat, 3);
  vbgmm.runEM(true);
  LOG(INFO) << vbgmm.numOfClusters();
  LOG(INFO) << vbgmm.centroids();
  std::map<double, double> overlapToCentroid;
  for (size_t r = 0; r < allOverlaps.size(); ++r) {
    overlapToCentroid[allOverlaps[r]] = vbgmm.centroids()(vbgmm.labels()(r), 0);
  }

  std::map<size_t, std::set<size_t>> idxToPrunedConn;
  for (const auto& [key, val] : idxToConn) {
    if (val.empty()) {
      throw ZException(QString("Can not do restitching because images are not connected. Abort."));
    }
    std::vector<std::pair<size_t, double>> valC = val;
    for (auto& p : valC) {
      CHECK(overlapToCentroid.contains(p.second));
      p.second = overlapToCentroid[p.second];
    }
    // VLOG(1) << valC.size();
    std::ranges::sort(valC, std::ranges::greater{}, &std::pair<size_t, double>::second);
    idxToPrunedConn[key].insert(valC[0].first);
    double lastOverlap = valC[0].second;
    for (size_t i = 1; i < valC.size(); ++i) { // keep two
      if (valC[i].second != lastOverlap) {
        break;
      }
      idxToPrunedConn[key].insert(valC[i].first);
      // VLOG(1) << valC[i].second;
    }
  }

  // prune
  for (const auto& [key, val] : conn) {
    if (idxToPrunedConn[key.first].contains(key.second) || idxToPrunedConn[key.second].contains(key.first)) {
      connRes[key] = val.first;
    }
  }
}

} // anonymous namespace

namespace nim {

ZStitchImage::ZStitchImage()
  : ZImgProcess()
{}

void ZStitchImage::setInputFilenames(const QStringList& fns, size_t scene)
{
  m_inputFilenames = fns;
  std::ranges::sort(m_inputFilenames, naturalSortLessThan);
  m_scene = scene;
}

void ZStitchImage::set2ndInput(const QStringList& fns,
                               size_t scene,
                               const std::vector<size_t>& useChs,
                               const std::vector<size_t>& chsForBackgroundRemove,
                               size_t commonChannelOfInput,
                               size_t commonChannelof2ndInput)
{
  m_2ndInputFilenames = fns;
  std::ranges::sort(m_2ndInputFilenames, naturalSortLessThan);
  m_2ndScene = scene;
  m_2ndChannelsToUse = useChs;
  m_2ndChannelsToRemoveBackground = chsForBackgroundRemove;
  m_commonChannelOfInput = commonChannelOfInput;
  m_commonChannelOf2ndInput = commonChannelof2ndInput;
}

void ZStitchImage::setTileGridFromTileSelectionImage(const QString& fn)
{
  unsetTileConfiguration();
  ZImg m_tileImage = ZImg(fn);
  getTileMatrixFromConnImage(m_tileImage);
}

void ZStitchImage::setTileGridFromMatrixFile(const QString& file)
{
  unsetTileConfiguration();
  auto mat = ZEigenUtils::readMatrix(file);
  ZImg img(ZImgInfo(mat.cols(), mat.rows(), 1, 1, 1, 4, VoxelFormat::Signed));
  img.fill(0);

  auto data = img.data<int32_t>(0);
  size_t idx = 0;
  for (Eigen::Index r = 0; r < mat.rows(); ++r) {
    for (Eigen::Index c = 0; c < mat.cols(); ++c) {
      data[idx++] = static_cast<int32_t>(mat(r, c));
    }
  }
  m_tileGrid = img;
}

void ZStitchImage::setTileGridFromLayout(size_t numRows, size_t numCols)
{
  unsetTileConfiguration();
  ZImg img(ZImgInfo(numCols, numRows, 1, 1, 1, 4, VoxelFormat::Signed));
  img.fill(0);

  auto data = img.data<int32_t>(0);
  int32_t idx = 0;
  for (size_t r = 0; r < numRows; ++r) {
    for (size_t c = 0; c < numCols; ++c) {
      data[idx] = idx + 1;
      ++idx;
    }
  }
  m_tileGrid = img;
}

void ZStitchImage::doWork()
{
  LOG(INFO) << "";
  LOG(INFO) << "Start Stitching";
  logLongString(toString());
  LOG(INFO) << "";

  QFileInfo outputFI(m_resFileName);
  if (m_resFileName.isEmpty() || !outputFI.absoluteDir().exists()) {
    throw ZException("Please make sure the output folder exists.");
  }
  if (m_inputFilenames.empty()) {
    throw ZException("No input image.");
  }
  bool hasStack2 = m_2ndInputFilenames.size() == m_inputFilenames.size();

  if (m_restitch) {
    if (hasStack2) {
      throw ZException("restitch and two sets of input stacks are not compatible.");
    }
    if (m_concatenateOnly) {
      throw ZException("restitch and concatenate-only are not compatible.");
    }
    doRestitch();
    return;
  }

  size_t nStacks = 0;
  std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint> conn;

  if (!m_connTextFile.isEmpty()) {
    nStacks = buildConnectionFromTextFile(m_connTextFile, conn);
  } else if (!m_tileGrid.isEmpty()) {
    nStacks = buildConnectionFromGrid(m_tileGrid, conn);
  } else {
    LOG(INFO) << "no connection configuration, blind stitching...";
    nStacks = m_inputFilenames.size();
    buildFullConnection(nStacks, conn);
  }

  if (hasStack2) {
    for (const auto& [key, val] : conn) {
      conn[std::make_pair(key.first, key.first + nStacks)] = ZImgNCCMatch::PositionHint::None;
      conn[std::make_pair(key.second, key.second + nStacks)] = ZImgNCCMatch::PositionHint::None;
      conn[std::make_pair(key.first + nStacks, key.second + nStacks)] = val;
    }
  }

  std::vector<ZImgSource> inputStackSources;
  std::vector<ZImgSource> input2ndStackSources;
  if (nStacks == 0) {
    throw ZException("No image to stitch.");
  } else if (nStacks == 1) {
    if (hasStack2) {
      inputStackSources.emplace_back(m_inputFilenames[0], ZImgRegion(), m_scene);
      input2ndStackSources.emplace_back(m_2ndInputFilenames[0], ZImgRegion(), m_2ndScene);
      auto info = ZImg::readImgInfo(inputStackSources[0]);
      auto tmpInfo = ZImg::readImgInfo(input2ndStackSources[0]);
      if (!tmpInfo.isSameType(info)) {
        throw ZException(fmt::format("Image type of {} <{}> and {} <{}> don't match",
                                     inputStackSources[0],
                                     info,
                                     input2ndStackSources[0],
                                     tmpInfo));
      }
    } else {
      if (m_downsampleBlockDepth > 1 || m_downsampleBlockWidth > 1 || m_downsampleBlockHeight > 1) {
        ZImg img(m_inputFilenames[0], ZImgRegion(), m_scene);
        LOG(INFO) << "Downsampling ...";
        img.blockDownsample(m_downsampleBlockWidth,
                            m_downsampleBlockHeight,
                            m_downsampleBlockDepth,
                            m_downsampleMergeMode);

        img.save(m_resFileName);

        LOG(INFO) << fmt::format("{} saved.", m_resFileName);
        return;
      }
      throw ZException("Need at least two images to do stitching.");
    }
  } else {
    if (nStacks == static_cast<size_t>(m_inputFilenames.size())) {
      // perfect
      for (const auto& filename : m_inputFilenames) {
        inputStackSources.emplace_back(filename, ZImgRegion(), m_scene);
      }
      auto info = ZImg::readImgInfo(inputStackSources[0]);
      for (size_t i = 1; i < inputStackSources.size(); ++i) {
        auto tmpInfo = ZImg::readImgInfo(inputStackSources[i]);
        if (!tmpInfo.isSameType(info)) {
          throw ZException(fmt::format("Image type of {} <{}> and {} <{}> don't match",
                                       inputStackSources[0],
                                       info,
                                       inputStackSources[i],
                                       tmpInfo));
        }
      }
      if (hasStack2) {
        for (const auto& filename : m_2ndInputFilenames) {
          input2ndStackSources.emplace_back(filename, ZImgRegion(), m_2ndScene);
        }
        for (auto& input2ndStackSource : input2ndStackSources) {
          auto tmpInfo = ZImg::readImgInfo(input2ndStackSource);
          if (!tmpInfo.isSameType(info)) {
            throw ZException(fmt::format("Image type of {} <{}> and {} <{}> don't match",
                                         inputStackSources[0],
                                         info,
                                         input2ndStackSource,
                                         tmpInfo));
          }
        }
      }
    } else if (m_inputFilenames.size() == 1) {
      // try to split the input image to nStacks images
      LOG(WARNING) << "trying to split the input image to " << nStacks << " parts. This can be wrong.";
      const auto& filename = m_inputFilenames[0];
      auto info = ZImg::readImgInfo(ZImgSource(filename, ZImgRegion(), m_scene));
      if (info.numTimes == nStacks) {
        LOG(WARNING) << "trying to split the input image by time points";
        for (size_t i = 0; i < nStacks; ++i) {
          ZImgRegion rgn;
          rgn.start.t = i;
          rgn.end.t = i + 1;
          inputStackSources.emplace_back(filename, rgn, m_scene);
        }
        if (hasStack2) {
          auto info2 = ZImg::readImgInfo(ZImgSource(m_2ndInputFilenames[0], ZImgRegion(), m_2ndScene));
          if (info2.numTimes == nStacks) {
            for (size_t i = 0; i < nStacks; ++i) {
              ZImgRegion rgn;
              rgn.start.t = i;
              rgn.end.t = i + 1;
              input2ndStackSources.emplace_back(m_2ndInputFilenames[0], rgn, m_2ndScene);
            }
          } else {
            throw ZException("can not split 2nd stack input in the same way");
          }
        }
      } else if (info.depth % nStacks == 0) {
        LOG(WARNING) << "trying to split the input image by depth";
        size_t depthPerStack = info.depth / nStacks;
        for (size_t i = 0; i < nStacks; ++i) {
          ZImgRegion rgn;
          rgn.start.z = depthPerStack * i;
          rgn.end.z = (i + 1) * depthPerStack;
          inputStackSources.emplace_back(filename, rgn, m_scene);
        }
        if (hasStack2) {
          auto info2 = ZImg::readImgInfo(ZImgSource(m_2ndInputFilenames[0], ZImgRegion(), m_2ndScene));
          if (info2.depth % nStacks == 0) {
            depthPerStack = info2.depth / nStacks;
            for (size_t i = 0; i < nStacks; ++i) {
              ZImgRegion rgn;
              rgn.start.z = depthPerStack * i;
              rgn.end.z = (i + 1) * depthPerStack;
              input2ndStackSources.emplace_back(m_2ndInputFilenames[0], rgn, m_2ndScene);
            }
          } else {
            throw ZException("can not split 2nd stack input in the same way");
          }
        }
      } else {
        throw ZException(fmt::format("do not know how to split the input image to {} parts.", nStacks));
      }
    } else {
      throw ZException(fmt::format("number of inputs {} does not match number of stacks {} to stitch.",
                                   m_inputFilenames.size(),
                                   nStacks));
    }

    LOG(INFO) << fmt::format("Stitching {} images ...", nStacks);
    for (const auto& ss : inputStackSources) {
      LOG(INFO) << ss;
    }
    if (hasStack2) {
      for (const auto& ss : input2ndStackSources) {
        LOG(INFO) << ss;
      }
      inputStackSources.insert(inputStackSources.end(), input2ndStackSources.begin(), input2ndStackSources.end());
    }
  }

  {
    // for every pair of img
    boost::concurrent_flat_map<std::pair<size_t, size_t>, std::pair<ZVoxelCoordinate, double>> offsets;
    ZImgInfo oneImgInfo = ZImg::readImgInfo(inputStackSources[0]);

    std::vector<std::tuple<size_t, size_t, ZImgNCCMatch::PositionHint>> allPairs;
    for (const auto& con : conn) {
      allPairs.emplace_back(con.first.first, con.first.second, con.second);
    }

    auto stitch_pair = [&](size_t i) {
      size_t f = std::get<0>(allPairs[i]);
      size_t m = std::get<1>(allPairs[i]);
      if (offsets.contains(std::make_pair(m, f))) {
        return;
      }
      ZImg fixedImg(inputStackSources[f]);
      ZImg movingImg(inputStackSources[m]);
      fixedImg.blockDownsample(m_downsampleBlockWidth,
                               m_downsampleBlockHeight,
                               m_downsampleBlockDepth,
                               m_downsampleMergeMode);
      movingImg.blockDownsample(m_downsampleBlockWidth,
                                m_downsampleBlockHeight,
                                m_downsampleBlockDepth,
                                m_downsampleMergeMode);

      ZImgNCCMatch imgNCCMatch(fixedImg, movingImg);
      ZImgNCCMatch::PositionHint hint = std::get<2>(allPairs[i]);
      imgNCCMatch.setMovingImgPositionHint(hint, m_maxOverlapRate);

      if (m_concatenateOnly) {
        if ((f < nStacks && m < nStacks) || (f >= nStacks && m >= nStacks)) {
          // within same input set
          ZVoxelCoordinate movingImgOffset = imgNCCMatch.getMovingImgOffsetFromHint(0., 0., 0.);
          offsets.insert_or_assign(std::make_pair(f, m), std::make_pair(movingImgOffset, 0));
        } else {
          // between input set
          CHECK(m == f + nStacks);
          // append channels of 2nd input set to channels of 1st input set
          offsets.insert_or_assign(std::make_pair(f, m),
                                   std::make_pair(ZVoxelCoordinate(0, 0, 0, oneImgInfo.numChannels), 0));
        }
      } else {
        if ((f < nStacks && m < nStacks) || (f >= nStacks && m >= nStacks)) {
          // within same input set
          const auto& chsToUse = f < nStacks ? m_channelsToUse : m_2ndChannelsToUse;
          const auto& chsToRemoveBackground =
            f < nStacks ? m_channelsToRemoveBackground : m_2ndChannelsToRemoveBackground;

          if (chsToUse.empty()) {
            imgNCCMatch.useAllFixedImgChannels();
            imgNCCMatch.useAllMovingImgChannels();
          } else {
            imgNCCMatch.useFixedImgChannels(chsToUse);
            imgNCCMatch.useMovingImgChannels(chsToUse);
          }
          imgNCCMatch.removeBackgroundForFixedImgChannels(chsToRemoveBackground);
          imgNCCMatch.removeBackgroundForMovingImgChannels(chsToRemoveBackground);

          double maxNCC;
          ZVoxelCoordinate movingImgOffset = imgNCCMatch.computeMovingImgOffsetMR(m_startResolutionIntvX,
                                                                                  m_startResolutionIntvY,
                                                                                  m_startResolutionIntvZ,
                                                                                  &maxNCC);
          offsets.insert_or_assign(std::make_pair(f, m), std::make_pair(movingImgOffset, maxNCC));

          LOG(INFO) << fmt::format("img {0} -- img {1}, img {1} position hint: {2}, offset: {3}, NCC: {4}",
                                   f + 1,
                                   m + 1,
                                   imgNCCMatch.positionHintToString(),
                                   movingImgOffset,
                                   maxNCC);
          ;
        } else {
          // between input set
          CHECK(m == f + nStacks);

          imgNCCMatch.useFixedImgChannels(std::vector<size_t>{m_commonChannelOfInput});
          imgNCCMatch.useMovingImgChannels(std::vector<size_t>{m_commonChannelOf2ndInput});
          imgNCCMatch.removeBackgroundForFixedImgChannels(m_channelsToRemoveBackground);
          imgNCCMatch.removeBackgroundForMovingImgChannels(m_2ndChannelsToRemoveBackground);

          double maxNCC;
          ZVoxelCoordinate movingImgOffset = imgNCCMatch.computeMovingImgOffsetMR(m_startResolutionIntvX,
                                                                                  m_startResolutionIntvY,
                                                                                  m_startResolutionIntvZ,
                                                                                  &maxNCC);
          // append moving img channels to fixed img channels
          movingImgOffset.c = oneImgInfo.numChannels;
          offsets.insert_or_assign(std::make_pair(f, m), std::make_pair(movingImgOffset, maxNCC));

          LOG(INFO) << fmt::format("img {0} -- img {1}, img {1} position hint: {2}, offset: {3}, NCC: {4}",
                                   f + 1,
                                   m + 1,
                                   imgNCCMatch.positionHintToString(),
                                   movingImgOffset,
                                   maxNCC);
        }
      }
    };

    if (m_useMultithreading) {
      FLAGS_zimg_global_fft_number_of_threads = 1;
      auto guard = folly::makeGuard([]() {
        FLAGS_zimg_global_fft_number_of_threads = 0;
      });
      //      int nthread =
      //        std::min<int>(tbb::task_scheduler_init::default_num_threads(),
      //                      std::floor(ZCpuInfo::instance().nPhysicalRAM * 1.0 / oneImgInfo.byteNumber() / 3.0));
      size_t nthread = std::floor(ZCpuInfo::instance().nPhysicalRAM * 1.0 / oneImgInfo.byteNumber() / 3.0);
      nthread = std::max(1_uz, nthread);
      LOG(INFO) << "using maximum " << nthread << " threads to stitch.";
      tbb::global_control gc(tbb::global_control::max_allowed_parallelism, nthread);

      tbb::parallel_for(tbb::blocked_range<size_t>(0, allPairs.size()), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i) {
          stitch_pair(i);
        }
      });
    } else {
      for (size_t i = 0; i < allPairs.size(); ++i) {
        stitch_pair(i);
      }
    }

    ZImgMerge imgMerge;
    std::vector<ZImgTileSubBlock> imgs;
    for (const auto& ss : inputStackSources) {
      imgs.emplace_back(ss,
                        m_downsampleBlockWidth,
                        m_downsampleBlockHeight,
                        m_downsampleBlockDepth,
                        m_downsampleMergeMode);
    }
    // for (const auto& fixedMovingOffsetCost : offsets) {
    //   size_t f = fixedMovingOffsetCost.first.first;
    //   size_t m = fixedMovingOffsetCost.first.second;
    //   imgMerge.addImgPair(imgs[f],
    //                       imgs[m],
    //                       fixedMovingOffsetCost.second.first,
    //                       -(fixedMovingOffsetCost.second.second),
    //                       QString::number(f + 1),
    //                       QString::number(m + 1));
    // }
    offsets.cvisit_all([&](const auto& fixedMovingOffsetCost) {
      size_t f = fixedMovingOffsetCost.first.first;
      size_t m = fixedMovingOffsetCost.first.second;
      imgMerge.addImgPair(imgs[f],
                          imgs[m],
                          fixedMovingOffsetCost.second.first,
                          -(fixedMovingOffsetCost.second.second),
                          QString::number(f + 1),
                          QString::number(m + 1));
    });

    imgMerge.setMergeMode(m_mergeMode);
    auto summary = imgMerge.resolveLocations();

    imgMerge.save(m_resFileName);
    if (true) {
      for (size_t c = 0; c < imgMerge.imgInfo().numChannels; ++c) {
        QFileInfo fi(m_resFileName);
        QString ofn = fi.path() + "/" + fi.baseName() + QString("_ch%1.v3draw").arg(c + 1);
        QString dsofn = fi.path() + "/" + fi.baseName() + QString("_ch%1_downsampled.v3draw").arg(c + 1);
        ZImg img(m_resFileName, ZImgRegion(0, -1, 0, -1, 0, -1, c, c + 1));
        img.save(ofn);
        img.blockDownsample(2, 2, 1, ImgMergeMode::Mean);
        img.save(dsofn);
      }
    }
  }

  LOG(INFO) << fmt::format("{} saved.", m_resFileName);
  Q_EMIT resultReady(m_resFileName);
}

void ZStitchImage::read(const json::object& jo)
{
  setInputFilenames(json::value_to<QStringList>(jo.at("input_files")),
                    json::value_to<size_t>(jo.at("input_files_scene")));

  setResultFilename(json::value_to<QString>(jo.at("result_file")));

  setUseMultithreading(jo.at("use_multithreading").as_bool());

  setUseChannels();
  if (jo.contains("channels_to_use")) {
    setUseChannels(json::value_to<std::vector<size_t>>(jo.at("channels_to_use")));
  }

  setRemoveBackgroundForChannels();
  if (jo.contains("channels_to_remove_background")) {
    setRemoveBackgroundForChannels(json::value_to<std::vector<size_t>>(jo.at("channels_to_remove_background")));
  }

  if (jo.contains("input_2_files")) {
    std::vector<size_t> chsToUse;
    std::vector<size_t> chsToRB;
    if (jo.contains("input_2_channels_to_use")) {
      chsToUse = json::value_to<std::vector<size_t>>(jo.at("input_2_channels_to_use"));
    }
    if (jo.contains("input_2_channels_to_remove_background")) {
      chsToRB = json::value_to<std::vector<size_t>>(jo.at("input_2_channels_to_remove_background"));
    }
    set2ndInput(json::value_to<QStringList>(jo.at("input_2_files")),
                json::value_to<size_t>(jo.at("input_2_files_scene")),
                chsToUse,
                chsToRB,
                json::value_to<size_t>(jo.at("input_common_channel")),
                json::value_to<size_t>(jo.at("input_2_common_channel")));
  }

  setMergeMode(stringToEnum<ImgMergeMode>(jo.at("merge_mode").as_string()));
  if (jo.contains("downsample_block_width")) {
    setDownsampleBeforeStitching(json::value_to<size_t>(jo.at("downsample_block_width")),
                                 json::value_to<size_t>(jo.at("downsample_block_height")),
                                 json::value_to<size_t>(jo.at("downsample_block_depth")),
                                 stringToEnum<ImgMergeMode>(jo.at("downsample_block_merge_mode").as_string()));
  }
  setStartResolution(json::value_to<size_t>(jo.at("start_resolution_intv_X")),
                     json::value_to<size_t>(jo.at("start_resolution_intv_Y")),
                     json::value_to<size_t>(jo.at("start_resolution_intv_Z")));
  if (jo.contains("concatenate_only") && jo.at("concatenate_only").as_bool()) {
    setConcatenateOnly();
  }
  setMaxOverlapRate(json::value_to<double>(jo.at("max_overlap_rate")));

  if (jo.contains("tile_grid")) {
    setTileGrid(json::value_to<ZImg>(jo.at("tile_grid")));
  } else if (jo.contains("conn_text_file")) {
    setConnInfoFromConnTextFile(json::value_to<QString>(jo.at("conn_text_file")));
  } else if (jo.contains("restitch") && jo.at("restitch").as_bool()) {
    setRestitch();
  } else {
    setBlindStitching();
  }
}

void ZStitchImage::write(json::object& jo) const
{
  jo["input_files"] = json::value_from(m_inputFilenames);

  jo["input_files_scene"] = m_scene;

  jo["result_file"] = json::value_from(m_resFileName);

  jo["use_multithreading"] = m_useMultithreading;

  if (!m_channelsToUse.empty()) {
    jo["channels_to_use"] = json::value_from(m_channelsToUse);
  }
  if (!m_channelsToRemoveBackground.empty()) {
    jo["channels_to_remove_background"] = json::value_from(m_channelsToRemoveBackground);
  }
  if (m_downsampleBlockDepth > 1 || m_downsampleBlockWidth > 1 || m_downsampleBlockHeight > 1) {
    jo["downsample_block_width"] = m_downsampleBlockWidth;
    jo["downsample_block_height"] = m_downsampleBlockHeight;
    jo["downsample_block_depth"] = m_downsampleBlockDepth;
    jo["downsample_block_merge_mode"] = enumToString(m_downsampleMergeMode);
  }
  jo["merge_mode"] = enumToString(m_mergeMode);
  if (m_concatenateOnly) {
    jo["concatenate_only"] = m_concatenateOnly;
  }
  jo["start_resolution_intv_X"] = m_startResolutionIntvX;
  jo["start_resolution_intv_Y"] = m_startResolutionIntvY;
  jo["start_resolution_intv_Z"] = m_startResolutionIntvZ;
  jo["max_overlap_rate"] = m_maxOverlapRate;

  if (!m_tileGrid.isEmpty()) {
    jo["tile_grid"] = json::value_from(m_tileGrid);
  } else if (!m_connTextFile.isEmpty()) {
    jo["conn_text_file"] = json::value_from(m_connTextFile);
  } else if (m_restitch) {
    jo["restitch"] = m_restitch;
  }

  if (!m_2ndInputFilenames.isEmpty()) {
    jo["input_2_files"] = json::value_from(m_2ndInputFilenames);

    jo["input_2_files_scene"] = m_2ndScene;

    if (!m_2ndChannelsToUse.empty()) {
      jo["input_2_channels_to_use"] = json::value_from(m_2ndChannelsToUse);
    }
    if (!m_2ndChannelsToRemoveBackground.empty()) {
      jo["input_2_channels_to_remove_background"] = json::value_from(m_2ndChannelsToRemoveBackground);
    }
    jo["input_common_channel"] = m_commonChannelOfInput;
    jo["input_2_common_channel"] = m_commonChannelOf2ndInput;
  }
}

void ZStitchImage::getTileMatrixFromConnImage(ZImg& img)
{
  double minvalue;
  double maxvalue;
  img.createView(0, 0).computeMinMax(minvalue, maxvalue);
  double midvalue = (minvalue + maxvalue) / 2;
  double thre1 = (minvalue + midvalue) / 2;
  double thre2 = (midvalue + maxvalue) / 2;
  size_t numCols = 0;
  size_t numRows = 0;
  for (size_t h = 0; h < img.height(); h++) {
    auto pre = minvalue;
    for (size_t w = 0; w < img.width(); w++) {
      if (img.value<double>(w, h, 0) > thre1 && img.value<double>(w, h, 0) > pre) {
        numCols++;
      }
      pre = img.value<double>(w, h, 0);
    }
    if (numCols > 0) {
      break;
    }
  }
  for (size_t w = 0; w < img.width(); w++) {
    auto pre = minvalue;
    for (size_t h = 0; h < img.height(); h++) {
      if (img.value<double>(w, h, 0) > thre1 && img.value<double>(w, h, 0) > pre) {
        numRows++;
      }
      pre = img.value<double>(w, h, 0);
    }
    if (numRows > 0) {
      break;
    }
  }
  if (numCols == 0 || numRows == 0) {
    m_tileGrid.clear();
    throw ZException("can not find any block in conn image");
  }

  m_tileGrid = ZImg(ZImgInfo(numCols, numRows, 1, 1, 1, 4, VoxelFormat::Signed));
  m_tileGrid.fill(0);
  int32_t tileindex = 1;
  size_t currentrow = 0;
  size_t currentcol = 0;
  for (size_t h = 1; h < img.height() - 1; h++) {
    for (size_t w = 1; w < img.width() - 1; w++) {
      auto value = img.value<int32_t>(w, h, 0);
      auto pre = img.value<int32_t>(w - 1, h, 0);
      auto up = img.value<int32_t>(w, h - 1, 0);
      if (value > thre1 && value > pre && value > up) {
        if (value > thre2) {
          if (currentrow >= numRows || currentcol >= numCols) {
            m_tileGrid.clear();
            throw ZException("can not parse tile conn image");
          }
          m_tileGrid.setValue(tileindex++, ZVoxelCoordinate(currentcol, currentrow));
        }
        currentcol++;
        if (currentcol >= numCols) {
          currentcol = 0;
          currentrow++;
        }
      }
    }
  }
}

void ZStitchImage::unsetTileConfiguration()
{
  m_restitch = false;
  m_tileGrid.clear();
  m_connTextFile.clear();
}

void ZStitchImage::doRestitch()
{
  if (m_inputFilenames.size() != 1 || !m_2ndInputFilenames.empty() ||
      !m_inputFilenames[0].endsWith(".czi", Qt::CaseInsensitive)) {
    throw ZException("restitching requires single czi file as input");
  }
  std::vector<std::vector<ZImgRegion>> rgnss = ZImg::getInternalSubRegions(m_inputFilenames[0]);
  if (m_scene >= rgnss.size()) {
    throw ZException("invalid scene");
  }
  const auto& rgns = rgnss[m_scene];

  std::vector<ZImgSource> inputStackSources;
  for (const auto& rgn : rgns) {
    inputStackSources.emplace_back(m_inputFilenames[0], rgn, m_scene);
  }
  std::map<std::pair<size_t, size_t>, ZVoxelCoordinate> conn;
  buildConnectionFromRegions(rgns, conn);

  {
    // for every pair of img
    boost::concurrent_flat_map<std::pair<size_t, size_t>, std::pair<ZVoxelCoordinate, double>> offsets;
    ZImgInfo oneImgInfo = ZImg::readImgInfo(inputStackSources[0]);

    std::vector<std::tuple<size_t, size_t, ZVoxelCoordinate>> allPairs;
    for (const auto& con : conn) {
      allPairs.emplace_back(con.first.first, con.first.second, con.second);
    }

    auto stitch_pair = [&](size_t i) {
      size_t f = std::get<0>(allPairs[i]);
      size_t m = std::get<1>(allPairs[i]);
      if (offsets.contains(std::make_pair(m, f))) {
        return;
      }
      ZImg fixedImg(inputStackSources[f]);
      ZImg movingImg(inputStackSources[m]);
      fixedImg.blockDownsample(m_downsampleBlockWidth,
                               m_downsampleBlockHeight,
                               m_downsampleBlockDepth,
                               m_downsampleMergeMode);
      movingImg.blockDownsample(m_downsampleBlockWidth,
                                m_downsampleBlockHeight,
                                m_downsampleBlockDepth,
                                m_downsampleMergeMode);

      ZImgNCCMatch imgNCCMatch(fixedImg, movingImg);
      ZVoxelCoordinate initOffset = std::get<2>(allPairs[i]);

      const auto& chsToUse = m_channelsToUse;
      const auto& chsToRemoveBackground = m_channelsToRemoveBackground;

      if (chsToUse.empty()) {
        imgNCCMatch.useAllFixedImgChannels();
        imgNCCMatch.useAllMovingImgChannels();
      } else {
        imgNCCMatch.useFixedImgChannels(chsToUse);
        imgNCCMatch.useMovingImgChannels(chsToUse);
      }
      imgNCCMatch.removeBackgroundForFixedImgChannels(chsToRemoveBackground);
      imgNCCMatch.removeBackgroundForMovingImgChannels(chsToRemoveBackground);

      size_t radiusX = std::ceil(m_maxOverlapRate / 5.0 * std::max(fixedImg.width(), movingImg.width()));
      size_t radiusY = std::ceil(m_maxOverlapRate / 5.0 * std::max(fixedImg.height(), movingImg.height()));
      size_t radiusZ = std::ceil(m_maxOverlapRate / 5.0 * std::max(fixedImg.depth(), movingImg.depth()));
      LOG(INFO) << fmt::format("radius: {}, {}, {}", radiusX, radiusY, radiusZ);

      double maxNCC;
      ZVoxelCoordinate movingImgOffset = imgNCCMatch.refineMovingImgOffsetMR(initOffset,
                                                                             radiusX,
                                                                             radiusY,
                                                                             radiusZ,
                                                                             m_startResolutionIntvX,
                                                                             m_startResolutionIntvY,
                                                                             m_startResolutionIntvZ,
                                                                             &maxNCC);
      offsets.insert_or_assign(std::make_pair(f, m), std::make_pair(movingImgOffset, maxNCC));

      LOG(INFO) << fmt::format(
        "tile {0} ({1}) -- tile {2} ({3}), tile {2} initial offset: {4}, final offset: {5}, NCC: {6}",
        f + 1,
        rgns[f].start,
        m + 1,
        rgns[m].start,
        initOffset,
        movingImgOffset,
        maxNCC);
    };

    if (m_useMultithreading) {
      FLAGS_zimg_global_fft_number_of_threads = 1;
      auto guard = folly::makeGuard([]() {
        FLAGS_zimg_global_fft_number_of_threads = 0;
      });
      //      int nthread =
      //        std::min<int>(tbb::task_scheduler_init::default_num_threads(),
      //                      std::floor(ZCpuInfo::instance().nPhysicalRAM * 1.0 / oneImgInfo.byteNumber() / 3.0));
      size_t nthread = std::floor(ZCpuInfo::instance().nPhysicalRAM * 1.0 / oneImgInfo.byteNumber() / 3.0);
      nthread = std::max(1_uz, nthread);
      LOG(INFO) << "using maximum " << nthread << " threads to stitch " << rgns.size() << " regions.";
      tbb::global_control gc(tbb::global_control::max_allowed_parallelism, nthread);

      tbb::parallel_for(tbb::blocked_range<size_t>(0, allPairs.size()), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i) {
          stitch_pair(i);
        }
      });
    } else {
      for (size_t i = 0; i < allPairs.size(); ++i) {
        stitch_pair(i);
      }
    }

    ZImgMerge imgMerge;
    std::vector<ZImgTileSubBlock> imgs;
    for (const auto& ss : inputStackSources) {
      imgs.emplace_back(ss,
                        m_downsampleBlockWidth,
                        m_downsampleBlockHeight,
                        m_downsampleBlockDepth,
                        m_downsampleMergeMode);
    }
    // for (const auto& fixedMovingOffsetCost : offsets) {
    //   size_t f = fixedMovingOffsetCost.first.first;
    //   size_t m = fixedMovingOffsetCost.first.second;
    //   imgMerge.addImgPair(imgs[f],
    //                       imgs[m],
    //                       fixedMovingOffsetCost.second.first,
    //                       -(fixedMovingOffsetCost.second.second),
    //                       QString::number(f + 1),
    //                       QString::number(m + 1));
    // }
    offsets.cvisit_all([&](const auto& fixedMovingOffsetCost) {
      size_t f = fixedMovingOffsetCost.first.first;
      size_t m = fixedMovingOffsetCost.first.second;
      imgMerge.addImgPair(imgs[f],
                          imgs[m],
                          fixedMovingOffsetCost.second.first,
                          -(fixedMovingOffsetCost.second.second),
                          QString::number(f + 1),
                          QString::number(m + 1));
    });

    imgMerge.setMergeMode(m_mergeMode);
    auto summary = imgMerge.resolveLocations();

    imgMerge.save(m_resFileName);
    if (true) {
      for (size_t c = 0; c < imgMerge.imgInfo().numChannels; ++c) {
        QFileInfo fi(m_resFileName);
        QString ofn = fi.path() + "/" + fi.baseName() + QString("_ch%1.v3draw").arg(c + 1);
        QString dsofn = fi.path() + "/" + fi.baseName() + QString("_ch%1_downsampled.v3draw").arg(c + 1);
        ZImg img(m_resFileName, ZImgRegion(0, -1, 0, -1, 0, -1, c, c + 1));
        img.save(ofn);
        img.blockDownsample(2, 2, 1, ImgMergeMode::Mean);
        img.save(dsofn);
      }
    }
  }

  LOG(INFO) << fmt::format("{} saved.", m_resFileName);
  Q_EMIT resultReady(m_resFileName);
}

} // namespace nim
