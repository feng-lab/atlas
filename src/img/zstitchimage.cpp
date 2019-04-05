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

void buildConnectionFromTextFile(const QString& filename,
                                 std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint>& conn)
{
  if (!QFile::exists(filename)) {
    throw ZImgException(QString("file %1 doesn't exist").arg(filename));
  }

  QStringList header;
  header << "# img1" << "img2" << "position";

  QRegularExpression rx(R"((\ |\,|\[|\]|\;))"); //RegEx for ' ' or ',' or '[' or ']' or ';'
  QFile inputFile(filename);
  if (inputFile.open(QIODevice::ReadOnly))
  {
    QTextStream in(&inputFile);
    while (!in.atEnd())
    {
      QString line = in.readLine();
      QStringList list = line.split(rx, QString::SkipEmptyParts);

      if (list.empty() || list.at(0).startsWith("#")) {
        continue;
      }
      if (list.size() < header.size()) {
        throw ZImgException(
          QString("Wrong number of items in line (%1), expected format: <%2>").arg(list.join(',')).arg(header.join(',')));
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
}

void buildConnectionFromGrid(const ZImg& grid,
                             std::map<std::pair<size_t, size_t>, nim::ZImgNCCMatch::PositionHint>& connRes)
{
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
            if (x + 1 < grid.width() && y + 1 < grid.height() && grid.value<int32_t>(x + 1, y + 1, z + 1) > 0) { //right down back
              size_t idx2 = grid.value<int32_t>(x + 1, y + 1, z + 1) - 1;
              std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
              conn[stackPair] = ZImgNCCMatch::PositionHint::Right | ZImgNCCMatch::PositionHint::Down | ZImgNCCMatch::PositionHint::Back;
              idxToMinConnDist[idx1] = std::min(3, idxToMinConnDist[idx1]);
              idxToMinConnDist[idx2] = std::min(3, idxToMinConnDist[idx2]);
              connDist[stackPair] = 3;
            }
            if (x > 0 && y + 1 < grid.height() && grid.value<int32_t>(x - 1, y + 1, z + 1) > 0) { //left down back
              size_t idx2 = grid.value<int32_t>(x - 1, y + 1, z + 1) - 1;
              std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
              conn[stackPair] = ZImgNCCMatch::PositionHint::Left | ZImgNCCMatch::PositionHint::Down | ZImgNCCMatch::PositionHint::Back;
              idxToMinConnDist[idx1] = std::min(3, idxToMinConnDist[idx1]);
              idxToMinConnDist[idx2] = std::min(3, idxToMinConnDist[idx2]);
              connDist[stackPair] = 3;
            }
            if (x + 1 < grid.width() && y > 0 && grid.value<int32_t>(x + 1, y - 1, z + 1) > 0) { //right up back
              size_t idx2 = grid.value<int32_t>(x + 1, y - 1, z + 1) - 1;
              std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
              conn[stackPair] = ZImgNCCMatch::PositionHint::Right | ZImgNCCMatch::PositionHint::Up | ZImgNCCMatch::PositionHint::Back;
              idxToMinConnDist[idx1] = std::min(3, idxToMinConnDist[idx1]);
              idxToMinConnDist[idx2] = std::min(3, idxToMinConnDist[idx2]);
              connDist[stackPair] = 3;
            }
            if (x > 0 && y > 0 && grid.value<int32_t>(x - 1, y - 1, z + 1) > 0) { //left up back
              size_t idx2 = grid.value<int32_t>(x - 1, y - 1, z + 1) - 1;
              std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
              conn[stackPair] = ZImgNCCMatch::PositionHint::Left | ZImgNCCMatch::PositionHint::Up | ZImgNCCMatch::PositionHint::Back;
              idxToMinConnDist[idx1] = std::min(3, idxToMinConnDist[idx1]);
              idxToMinConnDist[idx2] = std::min(3, idxToMinConnDist[idx2]);
              connDist[stackPair] = 3;
            }
          }
        }
      }
    }
  }

  for(const auto& [key, val] : idxToMinConnDist) {
    if (val > 3) {
      throw ZImgException(QString("Can not stitch because images are not connected. Abort."));
    }
  }

  // prune
  for (const auto& [key, val] : conn) {
    if (connDist[key] == idxToMinConnDist[key.first] ||
        connDist[key] == idxToMinConnDist[key.second]) {
      connRes[key] = val;
    }
  }
}

}  // anonymous namespace


namespace nim {

ZStitchImage::ZStitchImage()
  : ZImgProcess()
{
}

void ZStitchImage::setConnTileImage(const QString& fn)
{
  ZImg m_tileImage = ZImg(fn);
  getTileMatrixFromConnImage(m_tileImage);
}

void ZStitchImage::setTileGridFromMatrixFile(const QString& file)
{
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

void ZStitchImage::setConnInfoFromConnTextFile(const QString& file)
{
  m_connTextFile = file;
}

void ZStitchImage::doWork()
{
  QFileInfo outputFI(m_resFileName);
  if (m_resFileName.isEmpty() || !outputFI.absoluteDir().exists()) {
    throw ZImgException("Please make sure the ouput folder exists.");
  }
  if (m_inputStack1Filenames.empty()) {
    throw ZImgException("Please add input files.");
  }

  std::vector<ZImgInfo> stack1File1Infos = ZImg::readImgInfos(m_inputStack1Filenames[0]);
  for (size_t s = 1; s < stack1File1Infos.size(); ++s) {
    if (!stack1File1Infos[s].isSameType(stack1File1Infos[0])) {
      throw ZImgException(QString("Image type of %1 scene 0 <%2> and scene %3 <%4> don't match")
                            .arg(m_inputStack1Filenames[0]).arg(stack1File1Infos[0].toQString())
                            .arg(s).arg(stack1File1Infos[s].toQString()));
    }
  }
  for (int i = 1; i < m_inputStack1Filenames.size(); ++i) {
    std::vector<ZImgInfo> tmpInfos = ZImg::readImgInfos(m_inputStack1Filenames[i]);
    for (size_t s = 0; s < tmpInfos.size(); ++s) {
      if (!tmpInfos[s].isSameType(stack1File1Infos[0])) {
        throw ZImgException(QString("Image type of %1 <%2> and %3 <%4> don't match")
                              .arg(m_inputStack1Filenames[0]).arg(stack1File1Infos[0].toQString())
                              .arg(m_inputStack1Filenames[i]).arg(tmpInfos[s].toQString()));
      }
    }
  }

  size_t nstack;
  std::vector<ZImgSource> inputStackSources;

  nstack = m_inputStack1Filenames.size();
  for (int i = 0; i < m_inputStack1Filenames.size(); ++i) {
    inputStackSources.emplace_back(m_inputStack1Filenames[i]);
  }

  LOG(INFO) << QString("Stitching %1 images ...").arg(nstack);

  std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint> conn;

  /*generate connection file from tile_selection.lsm file*/
  std::vector<std::vector<size_t>> tileMatrix(m_tileMatrix.size(), std::vector<size_t>(m_tileMatrix[0].size(), 0));

  int index = 1;
  for (size_t i = 0; i < tileMatrix.size(); ++i) {
    for (size_t j = 0; j < tileMatrix[i].size(); ++j) {
      if (m_tileMatrix[i][j] > 0 && m_tileList[m_tileMatrix[i][j] - 1].bIsSelected) {
        tileMatrix[i][j] = index++;
      }
    }
  }
  buildConnectionFromGrid(tileMatrix, conn);

  for (size_t i = 0; i < inputStackSources.size(); ++i) {
    LOG(INFO) << inputStackSources[i].toQString();
  }

  {
    int intv[3];

    intv[0] = 1;
    intv[1] = 1;
    intv[2] = 1;

    // for every pair of img
    tbb::concurrent_unordered_map <std::pair<size_t, size_t>, std::pair<ZVoxelCoordinate, double>> offsets;
    ZImgInfo oneImgInfo = ZImg::readImgInfo(inputStackSources[0]);
    bool concurrent = ZCpuInfo::instance().nPhysicalRAM >= oneImgInfo.byteNumber() * 6;
    if (concurrent) {
      fftw_plan_with_nthreads(1);

      int nthread =
        std::min<int>(tbb::task_scheduler_init::default_num_threads(),
                      std::floor(ZCpuInfo::instance().nPhysicalRAM * 1.0 / oneImgInfo.byteNumber() / 3.0));
      LOG(INFO) << "using " << nthread << " threads to stitch.";
      tbb::task_scheduler_init init(nthread);

      std::vector<std::tuple<size_t, size_t, ZImgNCCMatch::PositionHint>> allPairs;
      for (const auto& con : conn) {
        allPairs.push_back(std::make_tuple(con.first.first, con.first.second, con.second));
      }
      tbb::parallel_for(
        tbb::blocked_range<size_t>(0, allPairs.size()),
        [&](const tbb::blocked_range <size_t>& r) {
          for (size_t i = r.begin(); i != r.end(); ++i) {
            size_t f = std::get<0>(allPairs[i]);
            size_t m = std::get<1>(allPairs[i]);
            if (offsets.find(std::make_pair(m, f)) != offsets.end()) {
              continue;
            }
            ZImg fixedImg(inputStackSources[f]);
            ZImg movingImg(inputStackSources[m]);

            ZImgNCCMatch imgNCCMatch(fixedImg, movingImg);

            if (m_channelsToUse.empty()) {
              imgNCCMatch.useAllFixedImgChannels();
              imgNCCMatch.useAllMovingImgChannels();
            } else {
              imgNCCMatch.useFixedImgChannel(m_channelsToUse);
              imgNCCMatch.useMovingImgChannel(m_channelsToUse);
            }

            ZImgNCCMatch::PositionHint hint = std::get<2>(allPairs[i]);
            imgNCCMatch.setMovingImgPositionHint(hint, m_maxOverlapRate);

            double maxNCC;
            ZVoxelCoordinate movingImgOffset = imgNCCMatch.computeMovingImgOffsetMR(intv[0], intv[1], intv[2],
                                                                                    &maxNCC);
            offsets[std::make_pair(f, m)] = std::make_pair(movingImgOffset, maxNCC);

            QString info = QString("img %1 -- img %2, img %2 position hint: %3, offset: %4, NCC: %5")
              .arg(f + 1).arg(m + 1).arg(imgNCCMatch.positionHintToQString()).arg(movingImgOffset.toQString()).arg(
              maxNCC);
            //m_commandOutputEdit->append(info);
            //QApplication::processEvents();
            LOG(INFO) << info;
          }
        }
      );

      fftw_plan_with_nthreads(ZCpuInfo::instance().nPhysicalCores);
    } else {
      for (size_t f = 0; f < nstack; ++f) {  // fixed
        ZImg fixedImg(inputStackSources[f]);
        for (size_t m = f + 1; m < nstack; ++m) { // moving
          // no connection
          if (!conn.empty() &&
              conn.find(std::make_pair(f, m)) == conn.end() &&
              conn.find(std::make_pair(m, f)) == conn.end()) {
            continue;
          }
          // already processed
          if (offsets.find(std::make_pair(m, f)) != offsets.end()) {
            continue;
          }

          ZImg movingImg(inputStackSources[m]);

          ZImgNCCMatch imgNCCMatch(fixedImg, movingImg);
          if (m_channelsToUse.empty()) {
            imgNCCMatch.useAllFixedImgChannels();
            imgNCCMatch.useAllMovingImgChannels();
          } else {
            imgNCCMatch.useFixedImgChannel(m_channelsToUse);
            imgNCCMatch.useMovingImgChannel(m_channelsToUse);
          }

          std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint>::iterator it = conn.find(
            std::make_pair(f, m));
          ZImgNCCMatch::PositionHint hint = ZImgNCCMatch::PositionHint::None;
          if (it != conn.end())
            hint = it->second;
          else {
            it = conn.find(std::make_pair(m, f));
            if (it != conn.end()) {
              hint = it->second;
              ZImgNCCMatch::reversePositionHint(hint);
            }
          }
          imgNCCMatch.setMovingImgPositionHint(hint, m_maxOverlapRate);

          double maxNCC;
          ZVoxelCoordinate movingImgOffset = imgNCCMatch.computeMovingImgOffsetMR(intv[0], intv[1], intv[2], &maxNCC);
          offsets[std::make_pair(f, m)] = std::make_pair(movingImgOffset, maxNCC);

          QString info = QString("img %1 -- img %2, img %2 position hint: %3, offset: %4, NCC: %5")
            .arg(f + 1).arg(m + 1).arg(imgNCCMatch.positionHintToQString()).arg(movingImgOffset.toQString()).arg(
            maxNCC);
          LOG(INFO) << info;
        }
      }
    }

    ZImgMerge imgMerge;
    std::vector<ZImgTileSubBlock> imgs;
    for (size_t i = 0; i < inputStackSources.size(); ++i) {
      imgs.emplace_back(inputStackSources[i]);
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
  setInputFilenames(readStringList(json, "input_files"));

  setResultFilename(readString(json, "result_file"));

  setUseChannels();
  if (json.contains("channels_to_use")) {
    std::vector<size_t> chs;
    auto numberArray = readNumberArray(json, "channels_to_use");
    chs.insert(m_channelsToUse.end(), numberArray.begin(), numberArray.end());
    setUseChannels(chs);
  }

  setMergeMode(stringToImgMergeMode(readString(json, "merge_mode")));
  setMaxOverlapRate(readNumber(json, "max_overlap_rate"));
  if (json.contains("tile_selection_image_file")) {
    setConnTileImage(readString(json, "tile_selection_image_file"));
  }
}

void ZStitchImage::write(QJsonObject& json) const
{
  json["input_files"] = QJsonArray::fromStringList(m_inputStack1Filenames);

  json["result_file"] = m_resFileName;

  if (!m_channelsToUse.empty()) {
    QJsonArray channelArray;
    for (auto ch : m_channelsToUse) {
      channelArray.append(QJsonValue(int(ch)));
    }
    json["channels_to_use"] = channelArray;
  }
  json["merge_mode"] = enumToString(m_mergeMode);
  json["max_overlap_rate"] = m_maxOverlapRate;
  if (!m_tileSelectionImageFilename.isEmpty()) {
    json["tile_selection_image_file"] = m_tileSelectionImageFilename;
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
      int post = img.value<int>(w + 1, h, 0);
      int down = img.value<int>(w, h + 1, 0);
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

} // namespace nim


