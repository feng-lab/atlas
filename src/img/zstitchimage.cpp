#include "zstitchimage.h"

#include "zlog.h"
#include "zimgnccmatch.h"
#include "zimgio.h"
#include "zeigenutils.h"
#include <QDir>
#include <QTextStream>
#include <QFileInfo>
#include <QRegularExpression>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/parallel_for.h>
#include <tbb/task_scheduler_init.h>
#include <fftw3.h>
#include <algorithm>
#include <limits>

namespace {

using namespace nim;

void buildFullConnection(size_t nStacks,
                         std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint>& conn)
{
  if (nStacks <= 1)
    return;

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

  if (!QFile::exists(filename)) {
    throw ZImgException(QString("file %1 doesn't exist").arg(filename));
  }

  QStringList header;
  header << "# img1" << "img2" << "position";

  QRegularExpression rx(R"((\ |\,|\[|\]|\;))"); //RegEx for ' ' or ',' or '[' or ']' or ';'
  QFile inputFile(filename);
  if (inputFile.open(QIODevice::ReadOnly)) {
    QTextStream in(&inputFile);
    while (!in.atEnd()) {
      QString line = in.readLine();
      QStringList list = line.split(rx, QString::SkipEmptyParts);

      if (list.empty() || list.at(0).startsWith("#")) {
        continue;
      }
      if (list.size() < header.size()) {
        throw ZImgException(
          QString("Wrong number of items in line (%1), expected format: <%2>").arg(list.join(',')).arg(
            header.join(',')));
      }
      bool ok = false;
      int idx1 = list[0].toInt(&ok);
      if (!ok || idx1 <= 0)
        throw ZImgException(
          QString("Can not parse line (%1) with format <%2>").arg(list.join(',')).arg(header.join(',')));
      int idx2 = list[1].toInt(&ok);
      if (!ok || idx2 <= 0)
        throw ZImgException(
          QString("Can not parse line (%1) with format <%2>").arg(list.join(',')).arg(header.join(',')));
      auto stackPair = std::make_pair<size_t, size_t>(idx1 - 1, idx2 - 1);
      nStacks = std::max<size_t>(nStacks, idx1);
      nStacks = std::max<size_t>(nStacks, idx2);
      conn[stackPair] = ZImgNCCMatch::PositionHint::None;
      for (int i = 2; i < list.size(); ++i) {
        QString pos = list[i].trimmed();
        if (pos.compare("Down", Qt::CaseInsensitive) == 0) {
          conn[stackPair] = conn[stackPair] | ZImgNCCMatch::PositionHint::Down;
        } else if (pos.compare("Right", Qt::CaseInsensitive) == 0) {
          conn[stackPair] = conn[stackPair] | ZImgNCCMatch::PositionHint::Right;
        } else if (pos.compare("Back", Qt::CaseInsensitive) == 0) {
          conn[stackPair] = conn[stackPair] | ZImgNCCMatch::PositionHint::Back;
        } else {
          throw ZImgException(
            QString("Can not parse line (%1) with format <%2>").arg(list.join(',')).arg(header.join(',')));
        }
      }
    }
    inputFile.close();
  }

  return nStacks;
}

size_t buildConnectionFromGrid(const ZImg& grid,
                               std::map<std::pair<size_t, size_t>, nim::ZImgNCCMatch::PositionHint>& connRes)
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

  std::map<std::pair<size_t, size_t>, nim::ZImgNCCMatch::PositionHint> conn;
  std::map<std::pair<size_t, size_t>, int> connDist;
  for (size_t z = 0; z < grid.depth(); ++z) {
    for (size_t y = 0; y < grid.height(); ++y) {
      for (size_t x = 0; x < grid.width(); ++x) {
        if (grid.value<int32_t>(x, y, z) > 0) {
          size_t idx1 = grid.value<int32_t>(x, y, z) - 1;

          if (x + 1 < grid.width() && grid.value<int32_t>(x + 1, y, z) > 0) { //right
            size_t idx2 = grid.value<int32_t>(x + 1, y, z) - 1;
            std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
            conn[stackPair] = ZImgNCCMatch::PositionHint::Right;
            idxToMinConnDist[idx1] = std::min(1, idxToMinConnDist[idx1]);
            idxToMinConnDist[idx2] = std::min(1, idxToMinConnDist[idx2]);
            connDist[stackPair] = 1;
          }
          if (y + 1 < grid.height() && grid.value<int32_t>(x, y + 1, z) > 0) { //down
            size_t idx2 = grid.value<int32_t>(x, y + 1, z) - 1;
            std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
            conn[stackPair] = ZImgNCCMatch::PositionHint::Down;
            idxToMinConnDist[idx1] = std::min(1, idxToMinConnDist[idx1]);
            idxToMinConnDist[idx2] = std::min(1, idxToMinConnDist[idx2]);
            connDist[stackPair] = 1;
          }
          if (z + 1 < grid.depth() && grid.value<int32_t>(x, y, z + 1) > 0) { //back
            size_t idx2 = grid.value<int32_t>(x, y, z + 1) - 1;
            std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
            conn[stackPair] = ZImgNCCMatch::PositionHint::Back;
            idxToMinConnDist[idx1] = std::min(1, idxToMinConnDist[idx1]);
            idxToMinConnDist[idx2] = std::min(1, idxToMinConnDist[idx2]);
            connDist[stackPair] = 1;
          }

          if (x + 1 < grid.width() && y + 1 < grid.height() && grid.value<int32_t>(x + 1, y + 1, z) > 0) { //right down
            size_t idx2 = grid.value<int32_t>(x + 1, y + 1, z) - 1;
            std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
            conn[stackPair] = ZImgNCCMatch::PositionHint::Right | ZImgNCCMatch::PositionHint::Down;
            idxToMinConnDist[idx1] = std::min(2, idxToMinConnDist[idx1]);
            idxToMinConnDist[idx2] = std::min(2, idxToMinConnDist[idx2]);
            connDist[stackPair] = 2;
          }
          if (x > 0 && y + 1 < grid.height() && grid.value<int32_t>(x - 1, y + 1, z) > 0) { //left down
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
                grid.value<int32_t>(x + 1, y + 1, z + 1) > 0) { //right down back
              size_t idx2 = grid.value<int32_t>(x + 1, y + 1, z + 1) - 1;
              std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
              conn[stackPair] =
                ZImgNCCMatch::PositionHint::Right | ZImgNCCMatch::PositionHint::Down | ZImgNCCMatch::PositionHint::Back;
              idxToMinConnDist[idx1] = std::min(3, idxToMinConnDist[idx1]);
              idxToMinConnDist[idx2] = std::min(3, idxToMinConnDist[idx2]);
              connDist[stackPair] = 3;
            }
            if (x > 0 && y + 1 < grid.height() && grid.value<int32_t>(x - 1, y + 1, z + 1) > 0) { //left down back
              size_t idx2 = grid.value<int32_t>(x - 1, y + 1, z + 1) - 1;
              std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
              conn[stackPair] =
                ZImgNCCMatch::PositionHint::Left | ZImgNCCMatch::PositionHint::Down | ZImgNCCMatch::PositionHint::Back;
              idxToMinConnDist[idx1] = std::min(3, idxToMinConnDist[idx1]);
              idxToMinConnDist[idx2] = std::min(3, idxToMinConnDist[idx2]);
              connDist[stackPair] = 3;
            }
            if (x + 1 < grid.width() && y > 0 && grid.value<int32_t>(x + 1, y - 1, z + 1) > 0) { //right up back
              size_t idx2 = grid.value<int32_t>(x + 1, y - 1, z + 1) - 1;
              std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
              conn[stackPair] =
                ZImgNCCMatch::PositionHint::Right | ZImgNCCMatch::PositionHint::Up | ZImgNCCMatch::PositionHint::Back;
              idxToMinConnDist[idx1] = std::min(3, idxToMinConnDist[idx1]);
              idxToMinConnDist[idx2] = std::min(3, idxToMinConnDist[idx2]);
              connDist[stackPair] = 3;
            }
            if (x > 0 && y > 0 && grid.value<int32_t>(x - 1, y - 1, z + 1) > 0) { //left up back
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

  for (const auto&[key, val] : idxToMinConnDist) {
    if (val > 3) {
      throw ZImgException(QString("Can not stitch because images are not connected. Abort."));
    }
  }

  // prune
  for (const auto&[key, val] : conn) {
    if (connDist[key] == idxToMinConnDist[key.first] ||
        connDist[key] == idxToMinConnDist[key.second]) {
      connRes[key] = val;
      nStacks = std::max(nStacks, key.first + 1);
      nStacks = std::max(nStacks, key.second + 1);
    }
  }

  return nStacks;
}

}  // anonymous namespace


namespace nim {

ZStitchImage::ZStitchImage()
  : ZImgProcess()
{
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
  size_t idx = 0;
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
  QFileInfo outputFI(m_resFileName);
  if (m_resFileName.isEmpty() || !outputFI.absoluteDir().exists()) {
    throw ZImgException("Please make sure the output folder exists.");
  }
  if (m_inputFilenames.empty()) {
    throw ZImgException("No input image.");
  }
  bool hasStack2 = m_2ndInputFilenames.size() == m_inputFilenames.size();

  if (m_restitch) {
    if (hasStack2) {
      throw ZImgException("restitch and two sets of input stacks are not compatible.");
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
    for (const auto&[key, val] : conn) {
      conn[std::make_pair(key.first, key.first + nStacks)] = ZImgNCCMatch::PositionHint::None;
      conn[std::make_pair(key.second, key.second + nStacks)] = ZImgNCCMatch::PositionHint::None;
      conn[std::make_pair(key.first + nStacks, key.second + nStacks)] = val;
    }
  }

  std::vector<ZImgSource> inputStackSources;
  std::vector<ZImgSource> input2ndStackSources;
  if (nStacks == 0) {
    throw ZImgException("No image to stitch.");
  } else if (nStacks == 1) {
    if (hasStack2) {
      inputStackSources.emplace_back(m_inputFilenames[0], ZImgRegion(), m_scene);
      input2ndStackSources.emplace_back(m_2ndInputFilenames[0], ZImgRegion(), m_2ndScene);
      auto info = ZImg::readImgInfo(inputStackSources[0]);
      auto tmpInfo = ZImg::readImgInfo(input2ndStackSources[0]);
      if (!tmpInfo.isSameType(info)) {
        throw ZImgException(QString("Image type of %1 <%2> and %3 <%4> don't match")
                              .arg(inputStackSources[0].toQString()).arg(info.toQString())
                              .arg(input2ndStackSources[0].toQString()).arg(tmpInfo.toQString()));
      }
    } else {
      if (m_downsampleBlockDepth > 1 || m_downsampleBlockWidth > 1 || m_downsampleBlockHeight > 1) {
        ZImg img(m_inputFilenames[0], ZImgRegion(), m_scene);
        LOG(INFO) << "Downsampling ...";
        img.blockDownsample(m_downsampleBlockWidth, m_downsampleBlockHeight, m_downsampleBlockDepth,
                            m_downsampleMergeMode);

        img.save(m_resFileName);

        LOG(INFO) << QString("%1 saved.").arg(m_resFileName);
        return;
      }
      throw ZImgException("Need at least two images to do stitching.");
    }
  } else {
    if (nStacks == size_t(m_inputFilenames.size())) {
      // perfect
      for (const auto& filename : m_inputFilenames) {
        inputStackSources.emplace_back(filename, ZImgRegion(), m_scene);
      }
      auto info = ZImg::readImgInfo(inputStackSources[0]);
      for (size_t i = 1; i < inputStackSources.size(); ++i) {
        auto tmpInfo = ZImg::readImgInfo(inputStackSources[i]);
        if (!tmpInfo.isSameType(info)) {
          throw ZImgException(QString("Image type of %1 <%2> and %3 <%4> don't match")
                                .arg(inputStackSources[0].toQString()).arg(info.toQString())
                                .arg(inputStackSources[i].toQString()).arg(tmpInfo.toQString()));
        }
      }
      if (hasStack2) {
        for (const auto& filename : m_2ndInputFilenames) {
          input2ndStackSources.emplace_back(filename, ZImgRegion(), m_2ndScene);
        }
        for (size_t i = 0; i < input2ndStackSources.size(); ++i) {
          auto tmpInfo = ZImg::readImgInfo(input2ndStackSources[i]);
          if (!tmpInfo.isSameType(info)) {
            throw ZImgException(QString("Image type of %1 <%2> and %3 <%4> don't match")
                                  .arg(inputStackSources[0].toQString()).arg(info.toQString())
                                  .arg(input2ndStackSources[i].toQString()).arg(tmpInfo.toQString()));
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
            throw ZImgException("can not split 2nd stack input in the same way");
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
            throw ZImgException("can not split 2nd stack input in the same way");
          }
        }
      } else {
        throw ZImgException(QString("do not know how to split the input image to %1 parts.").arg(nStacks));
      }
    } else {
      throw ZImgException(
        QString("number of inputs %1 does not match number of stacks %2 to stitch.").arg(
          m_inputFilenames.size()).arg(nStacks));
    }

    LOG(INFO) << QString("Stitching %1 images ...").arg(nStacks);
    for (const auto& ss : inputStackSources) {
      LOG(INFO) << ss.toQString();
    }
    if (hasStack2) {
      for (const auto& ss : input2ndStackSources) {
        LOG(INFO) << ss.toQString();
      }
      inputStackSources.insert(inputStackSources.end(),
                               input2ndStackSources.begin(),
                               input2ndStackSources.end());
    }
  }

  {
    // for every pair of img
    tbb::concurrent_unordered_map<std::pair<size_t, size_t>, std::pair<ZVoxelCoordinate, double>> offsets;
    ZImgInfo oneImgInfo = ZImg::readImgInfo(inputStackSources[0]);

    fftw_plan_with_nthreads(1);

    int nthread =
      std::min<int>(tbb::task_scheduler_init::default_num_threads(),
                    std::floor(ZCpuInfo::instance().nPhysicalRAM * 1.0 / oneImgInfo.byteNumber() / 3.0));
    nthread = std::max(1, nthread);
    LOG(INFO) << "using " << nthread << " threads to stitch.";
    tbb::task_scheduler_init init(nthread);

    std::vector<std::tuple<size_t, size_t, ZImgNCCMatch::PositionHint>> allPairs;
    for (const auto& con : conn) {
      allPairs.push_back(std::make_tuple(con.first.first, con.first.second, con.second));
    }
    tbb::parallel_for(
      tbb::blocked_range<size_t>(0, allPairs.size()),
      [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i) {
          size_t f = std::get<0>(allPairs[i]);
          size_t m = std::get<1>(allPairs[i]);
          if (offsets.find(std::make_pair(m, f)) != offsets.end()) {
            continue;
          }
          ZImg fixedImg(inputStackSources[f]);
          ZImg movingImg(inputStackSources[m]);
          fixedImg.blockDownsample(m_downsampleBlockWidth, m_downsampleBlockHeight, m_downsampleBlockDepth,
                                   m_downsampleMergeMode);
          movingImg.blockDownsample(m_downsampleBlockWidth, m_downsampleBlockHeight, m_downsampleBlockDepth,
                                    m_downsampleMergeMode);

          ZImgNCCMatch imgNCCMatch(fixedImg, movingImg);
          ZImgNCCMatch::PositionHint hint = std::get<2>(allPairs[i]);
          imgNCCMatch.setMovingImgPositionHint(hint, m_maxOverlapRate);

          if (m_concatenateOnly) {
            if ((f < nStacks && m < nStacks) || (f >= nStacks && m >= nStacks)) {
              // within same input set
              ZVoxelCoordinate movingImgOffset = imgNCCMatch.getMovingImgOffsetFromHint(0., 0., 0.);
              offsets[std::make_pair(f, m)] = std::make_pair(movingImgOffset, 0);
            } else {
              // between input set
              CHECK(m == f + nStacks);
              // append channels of 2nd input set to channels of 1st input set
              offsets[std::make_pair(f, m)] = std::make_pair(ZVoxelCoordinate(0, 0, 0, oneImgInfo.numChannels), 0);
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
                imgNCCMatch.useFixedImgChannel(chsToUse);
                imgNCCMatch.useMovingImgChannel(chsToUse);
              }

              if (!chsToRemoveBackground.empty()) {
                for (auto ch : chsToRemoveBackground) {
                  imgNCCMatch.enableRemoveBackgroundForFixedImgChannel(ch);
                  imgNCCMatch.enableRemoveBackgroundForMovingImgChannel(ch);
                }
              }

              double maxNCC;
              ZVoxelCoordinate movingImgOffset = imgNCCMatch.computeMovingImgOffsetMR(m_startResolutionIntvX,
                                                                                      m_startResolutionIntvY,
                                                                                      m_startResolutionIntvZ,
                                                                                      &maxNCC);
              offsets[std::make_pair(f, m)] = std::make_pair(movingImgOffset, maxNCC);

              QString info = QString("img %1 -- img %2, img %2 position hint: %3, offset: %4, NCC: %5")
                .arg(f + 1).arg(m + 1).arg(imgNCCMatch.positionHintToQString()).arg(movingImgOffset.toQString()).arg(
                maxNCC);

              LOG(INFO) << info;
            } else {
              // between input set
              CHECK(m == f + nStacks);

              imgNCCMatch.useFixedImgChannel(m_commonChannelOfInput);
              imgNCCMatch.useMovingImgChannel(m_commonChannelOf2ndInput);
              for (auto ch : m_channelsToRemoveBackground) {
                imgNCCMatch.enableRemoveBackgroundForFixedImgChannel(ch);
              }
              for (auto ch : m_2ndChannelsToRemoveBackground) {
                imgNCCMatch.enableRemoveBackgroundForMovingImgChannel(ch);
              }

              double maxNCC;
              ZVoxelCoordinate movingImgOffset = imgNCCMatch.computeMovingImgOffsetMR(m_startResolutionIntvX,
                                                                                      m_startResolutionIntvY,
                                                                                      m_startResolutionIntvZ,
                                                                                      &maxNCC);
              // append moving img channels to fixed img channels
              movingImgOffset.c = oneImgInfo.numChannels;
              offsets[std::make_pair(f, m)] = std::make_pair(movingImgOffset, maxNCC);

              QString info = QString("img %1 -- img %2, img %2 position hint: %3, offset: %4, NCC: %5")
                .arg(f + 1).arg(m + 1).arg(imgNCCMatch.positionHintToQString()).arg(movingImgOffset.toQString()).arg(
                maxNCC);

              LOG(INFO) << info;
            }
          }
        }
      }
    );

    fftw_plan_with_nthreads(ZCpuInfo::instance().nPhysicalCores);


    ZImgMerge imgMerge;
    std::vector<ZImgTileSubBlock> imgs;
    for (const auto& ss : inputStackSources) {
      imgs.emplace_back(ss, m_downsampleBlockWidth, m_downsampleBlockHeight, m_downsampleBlockDepth,
                        m_downsampleMergeMode);
    }
    for (const auto& fixedMovingOffsetCost : offsets) {
      size_t f = fixedMovingOffsetCost.first.first;
      size_t m = fixedMovingOffsetCost.first.second;
      imgMerge.addImgPair(imgs[f], imgs[m], fixedMovingOffsetCost.second.first,
                          -(fixedMovingOffsetCost.second.second),
                          QString::number(f + 1), QString::number(m + 1));
    }

    imgMerge.setMergeMode(m_mergeMode);
    QStringList summary = imgMerge.resolveLocations();

#if 0
    QString stitchInfoOutputName = m_resFileName;
    stitchInfoOutputName.append("_info.txt");
    QFile fOut(stitchInfoOutputName);
    if (fOut.open(QFile::WriteOnly | QFile::Text)) {
      QTextStream s(&fOut);
      for (const auto& mes : summary)
        s << mes << '\n';
    }
    fOut.close();
#endif
    if (imgMerge.imgInfo().byteNumber() * 3 > ZCpuInfo::instance().nPhysicalRAM &&
        m_mergeMode == ImgMergeMode::Max) {
      ZImgIO().writeImg(m_resFileName, imgMerge);
      for (size_t c = 0; c < imgMerge.imgInfo().numChannels; ++c) {
        QFileInfo fi(m_resFileName);
        QString ofn = fi.path() + "/" + fi.baseName() + QString("_ch%1.v3draw").arg(c + 1);
        QString dsofn = fi.path() + "/" + fi.baseName() + QString("_ch%1_downsampled.v3draw").arg(c + 1);
        ZImg img(m_resFileName, ZImgRegion(0, -1, 0, -1, 0, -1, c, c + 1));
        img.save(ofn);
        img.blockDownsample(2, 2, 1, ImgMergeMode::Mean);
        img.save(dsofn);
      }
    } else {
      auto wholeImg = imgMerge.wholeImg();
      wholeImg.save(m_resFileName);
      for (size_t c = 0; c < imgMerge.imgInfo().numChannels; ++c) {
        QFileInfo fi(m_resFileName);
        QString ofn = fi.path() + "/" + fi.baseName() + QString("_ch%1.v3draw").arg(c + 1);
        QString dsofn = fi.path() + "/" + fi.baseName() + QString("_ch%1_downsampled.v3draw").arg(c + 1);
        ZImg tmp = wholeImg.createView(c);
        tmp.save(ofn);
        tmp.blockDownsample(2, 2, 1, ImgMergeMode::Mean);
        tmp.save(dsofn);
      }
    }
  }

  LOG(INFO) << QString("%1 saved.").arg(m_resFileName);
}

void ZStitchImage::read(const QJsonObject& json)
{
  setInputFilenames(readStringList(json, "input_files"), readNumber(json, "input_files_scene"));

  setResultFilename(readString(json, "result_file"));

  setUseChannels();
  if (json.contains("channels_to_use")) {
    std::vector<size_t> chs;
    auto numberArray = readNumberArray(json, "channels_to_use");
    chs.insert(chs.end(), numberArray.begin(), numberArray.end());
    setUseChannels(chs);
  }

  setRemoveBackgroundForChannels();
  if (json.contains("channels_to_remove_background")) {
    std::vector<size_t> chs;
    auto numberArray = readNumberArray(json, "channels_to_remove_background");
    chs.insert(chs.end(), numberArray.begin(), numberArray.end());
    setRemoveBackgroundForChannels(chs);
  }

  if (json.contains("input_2_files")) {
    std::vector<size_t> chsToUse;
    std::vector<size_t> chsToRB;
    if (json.contains("input_2_channels_to_use")) {
      auto numberArray = readNumberArray(json, "input_2_channels_to_use");
      chsToUse.insert(chsToUse.end(), numberArray.begin(), numberArray.end());
    }
    if (json.contains("input_2_channels_to_remove_background")) {
      auto numberArray = readNumberArray(json, "input_2_channels_to_remove_background");
      chsToRB.insert(chsToRB.end(), numberArray.begin(), numberArray.end());
    }
    set2ndInput(readStringList(json, "input_2_files"), readNumber(json, "input_2_files_scene"),
                chsToUse, chsToRB, readNumber(json, "input_common_channel"),
                readNumber(json, "input_2_common_channel"));
  }

  setMergeMode(stringToImgMergeMode(readString(json, "merge_mode")));
  if (json.contains("downsample_block_width")) {
    setDownsampleBeforeStitching(readNumber(json, "downsample_block_width"),
                                 readNumber(json, "downsample_block_height"),
                                 readNumber(json, "downsample_block_depth"),
                                 stringToImgMergeMode(readString(json, "downsample_block_merge_mode")));
  }
  setStartResolution(readNumber(json, "start_resolution_intv_X"),
                     readNumber(json, "start_resolution_intv_Y"),
                     readNumber(json, "start_resolution_intv_Z"));
  if (json.contains("concatenate_only") && readBool(json, "concatenate_only")) {
    setConcatenateOnly();
  }
  setMaxOverlapRate(readNumber(json, "max_overlap_rate"));

  if (json.contains("tile_grid")) {
    setTileGrid(ZImg::fromJson(json["tile_grid"]));
  } else if (json.contains("conn_text_file")) {
    setConnInfoFromConnTextFile(readString(json, "conn_text_file"));
  } else if (json.contains("restitch") && readBool(json, "restitch")) {
    setRestitch();
  } else {
    setBlindStitching();
  }
}

void ZStitchImage::write(QJsonObject& json) const
{
  json["input_files"] = QJsonArray::fromStringList(m_inputFilenames);

  json["input_files_scene"] = int(m_scene);

  json["result_file"] = m_resFileName;

  if (!m_channelsToUse.empty()) {
    QJsonArray channelArray;
    for (auto ch : m_channelsToUse) {
      channelArray.append(QJsonValue(int(ch)));
    }
    json["channels_to_use"] = channelArray;
  }
  if (!m_channelsToRemoveBackground.empty()) {
    QJsonArray channelArray;
    for (auto ch : m_channelsToRemoveBackground) {
      channelArray.append(QJsonValue(int(ch)));
    }
    json["channels_to_remove_background"] = channelArray;
  }
  if (m_downsampleBlockDepth > 1 || m_downsampleBlockWidth > 1 || m_downsampleBlockHeight > 1) {
    json["downsample_block_width"] = (int)m_downsampleBlockWidth;
    json["downsample_block_height"] = (int)m_downsampleBlockHeight;
    json["downsample_block_depth"] = (int)m_downsampleBlockDepth;
    json["downsample_block_merge_mode"] = enumToString(m_downsampleMergeMode);
  }
  json["merge_mode"] = enumToString(m_mergeMode);
  if (m_concatenateOnly) {
    json["concatenate_only"] = m_concatenateOnly;
  }
  json["start_resolution_intv_X"] = (int)m_startResolutionIntvX;
  json["start_resolution_intv_Y"] = (int)m_startResolutionIntvY;
  json["start_resolution_intv_Z"] = (int)m_startResolutionIntvZ;
  json["max_overlap_rate"] = m_maxOverlapRate;

  if (!m_tileGrid.isEmpty()) {
    json["tile_grid"] = m_tileGrid.toJson();
  } else  if (!m_connTextFile.isEmpty()) {
    json["conn_text_file"] = m_connTextFile;
  } else if (m_restitch) {
    json["restitch"] = m_restitch;
  }

  if (!m_2ndInputFilenames.isEmpty()) {
    json["input_2_files"] = QJsonArray::fromStringList(m_2ndInputFilenames);

    json["input_2_files_scene"] = int(m_2ndScene);

    if (!m_2ndChannelsToUse.empty()) {
      QJsonArray channelArray;
      for (auto ch : m_2ndChannelsToUse) {
        channelArray.append(QJsonValue(int(ch)));
      }
      json["input_2_channels_to_use"] = channelArray;
    }
    if (!m_2ndChannelsToRemoveBackground.empty()) {
      QJsonArray channelArray;
      for (auto ch : m_2ndChannelsToRemoveBackground) {
        channelArray.append(QJsonValue(int(ch)));
      }
      json["input_2_channels_to_remove_background"] = channelArray;
    }
    json["input_common_channel"] = int(m_commonChannelOfInput);
    json["input_2_common_channel"] = int(m_commonChannelOf2ndInput);
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
    int pre = minvalue;
    for (size_t w = 0; w < img.width(); w++) {
      if (img.value<double>(w, h, 0) > thre1 && img.value<double>(w, h, 0) > pre) {
        numCols++;
      }
      pre = img.value<double>(w, h, 0);
    }
    if (numCols > 0)
      break;
  }
  for (size_t w = 0; w < img.width(); w++) {
    int pre = minvalue;
    for (size_t h = 0; h < img.height(); h++) {
      if (img.value<double>(w, h, 0) > thre1 && img.value<double>(w, h, 0) > pre) {
        numRows++;
      }
      pre = img.value<double>(w, h, 0);
    }
    if (numRows > 0)
      break;
  }
  if (numCols == 0 || numRows == 0) {
    m_tileGrid.clear();
    throw ZIOException("can not find any block in conn image");
  }

  m_tileGrid = ZImg(ZImgInfo(numCols, numRows, 1, 1, 1, 4, VoxelFormat::Signed));
  m_tileGrid.fill(0);
  int tileindex = 1;
  size_t currentrow = 0;
  size_t currentcol = 0;
  for (size_t h = 1; h < img.height() - 1; h++) {
    for (size_t w = 1; w < img.width() - 1; w++) {
      int value = img.value<int>(w, h, 0);
      int pre = img.value<int>(w - 1, h, 0);
      int up = img.value<int>(w, h - 1, 0);
      if (value > thre1 && value > pre && value > up) {
        if (value > thre2) {
          if (currentrow >= numRows || currentcol >= numCols) {
            m_tileGrid.clear();
            throw ZIOException("can not parse tile conn image");
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
  // todo
}

} // namespace nim


