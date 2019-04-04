#include "zstitchimage.h"

#include "zlog.h"
#include "zimgnccmatch.h"
#include "zimgio.h"
#include <QDir>
#include <QTextStream>
#include <QFileInfo>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/parallel_for.h>
#include <tbb/task_scheduler_init.h>
#include <fftw3.h>
#include <algorithm>
#include <limits>

namespace {

void buildConnectionFromGrid(const std::vector<std::vector<size_t>>& grid,
                             std::map<std::pair<size_t, size_t>, nim::ZImgNCCMatch::PositionHint>& conn)
{
  for (size_t i = 0; i < grid.size(); ++i) {
    for (size_t j = 0; j < grid[i].size(); ++j) {
      if (grid[i][j] > 0) {
        bool connected = false;
        if (j + 1 < grid[0].size() && grid[i][j + 1] > 0) { //right
          size_t idx1 = grid[i][j] - 1;
          size_t idx2 = grid[i][j + 1] - 1;
          std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
          conn[stackPair] = nim::ZImgNCCMatch::PositionHint::Right;
          connected = true;
        }
        if (i + 1 < grid.size() && grid[i + 1][j] > 0) { //down
          size_t idx1 = grid[i][j] - 1;
          size_t idx2 = grid[i + 1][j] - 1;
          std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
          conn[stackPair] = nim::ZImgNCCMatch::PositionHint::Down;
          connected = true;
        }
        if (i + 1 < grid.size() && j + 1 < grid[0].size() && grid[i + 1][j + 1] > 0
            && connected == false) {  // down-right, add only if right and down are empty
          size_t idx1 = grid[i][j] - 1;
          size_t idx2 = grid[i + 1][j + 1] - 1;
          std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
          conn[stackPair] = nim::ZImgNCCMatch::PositionHint::Down | nim::ZImgNCCMatch::PositionHint::Right;
          connected = true;
        }
        if (i + 1 < grid.size() && j >= 1 && grid[i + 1][j - 1] > 0
            && grid[i][j - 1] == 0 && grid[i + 1][j] == 0) {  // down-left, add only if left and down are empty
          size_t idx1 = grid[i][j] - 1;
          size_t idx2 = grid[i + 1][j - 1] - 1;
          std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
          conn[stackPair] = nim::ZImgNCCMatch::PositionHint::Down | nim::ZImgNCCMatch::PositionHint::Left;
          connected = true;
        }
        if (!connected) {  // test if this image is connected
          if (i >= 1 && j >= 1 && grid[i - 1][j - 1] > 0) {  // up-left
            size_t idx1 = grid[i][j] - 1;
            size_t idx2 = grid[i - 1][j - 1] - 1;
            if (conn.find(std::make_pair(idx2, idx1)) != conn.end())
              connected = true;
          }
          if (j >= 1 && grid[i][j - 1] > 0) { // left
            size_t idx1 = grid[i][j] - 1;
            size_t idx2 = grid[i][j - 1] - 1;
            if (conn.find(std::make_pair(idx2, idx1)) != conn.end())
              connected = true;
          }
          if (i >= 1 && grid[i - 1][j] > 0) { //up
            size_t idx1 = grid[i][j] - 1;
            size_t idx2 = grid[i - 1][j] - 1;
            if (conn.find(std::make_pair(idx2, idx1)) != conn.end())
              connected = true;
          }
          if (i >= 1 && j + 1 < grid[0].size() && grid[i - 1][j + 1] > 0) {  // up-right
            size_t idx1 = grid[i][j] - 1;
            size_t idx2 = grid[i - 1][j + 1] - 1;
            if (conn.find(std::make_pair(idx2, idx1)) != conn.end())
              connected = true;
          }
        }
        if (!connected) {
          throw nim::ZImgException(QString("Can not stitch because images are not connected. Abort."));
        }
      }
    }
  }
}

}  // anonymous namespace


namespace nim {

ZTile::ZTile(int index_, QPoint topleft, QPoint bottomright)
  : index(index_)
{
  region = QRect(topleft, bottomright);
}

ZStitchImage::ZStitchImage()
  : ZImgProcess()
{
}

void ZStitchImage::setConnTileImage(const QString& fn)
{
  if (!fn.isEmpty()) {
    m_tileSelectionImageFilename = fn;
    m_tileList.clear();

    ZImg m_tileImage = ZImg(fn);

    if (!getTileMatrix(m_tileImage, m_tileMatrix, m_tileList)) {
      m_tileList.clear();
      throw ZImgException(tr("Failed to parse tile connection image."));
    }
  }
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

  CHECK(int(nstack) == m_tileList.size());

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
        img.blockDownsample(2, 2, 1, ZImg::CombineMode::Mean);
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
        tmp.blockDownsample(2, 2, 1, ZImg::CombineMode::Mean);
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

  setUseAllChannels();
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

bool ZStitchImage::getTileMatrix(ZImg& img, std::vector<std::vector<int>>& tileMatrix, QList<ZTile>& tileList)
{
  double minvalue;
  double maxvalue;
  img.createView(0, 0).computeMinMax(minvalue, maxvalue);
  double midvalue = (minvalue + maxvalue) / 2;
  double thre1 = (minvalue + midvalue) / 2;
  double thre2 = (midvalue + maxvalue) / 2;
  size_t numTilePerRow = 0;
  size_t numTilePerCol = 0;
  tileMatrix.clear();
  tileList.clear();
  for (size_t h = 0; h < img.height(); h++) {
    int pre = minvalue;
    for (size_t w = 0; w < img.width(); w++) {
      if (img.value<double>(w, h, 0) > thre1 && img.value<double>(w, h, 0) > pre) {
        numTilePerRow++;
      }
      pre = img.value<double>(w, h, 0);
    }
    if (numTilePerRow > 0)
      break;
  }
  for (size_t w = 0; w < img.width(); w++) {
    int pre = minvalue;
    for (size_t h = 0; h < img.height(); h++) {
      if (img.value<double>(w, h, 0) > thre1 && img.value<double>(w, h, 0) > pre) {
        numTilePerCol++;
      }
      pre = img.value<double>(w, h, 0);
    }
    if (numTilePerCol > 0)
      break;
  }
  if (numTilePerRow == 0 || numTilePerCol == 0) {
    return false;
  }
  tileMatrix = std::vector<std::vector<int>>(numTilePerCol, std::vector<int>(numTilePerRow, 0));
  int tileindex = 1;
  int tileindex2 = 1;
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
          if (currentrow + 1 > tileMatrix.size() || currentcol + 1 > tileMatrix[currentrow].size()) {
            return false;
          }
          tileMatrix[currentrow][currentcol] = tileindex++;
          QPoint qp(w, h);
          ZTile tile(tileindex - 1, qp, qp);
          tileList.push_back(tile);
        }
        currentcol++;
        if (currentcol >= numTilePerRow) {
          currentcol = 0;
          currentrow++;
        }
      }
      if (value > thre2 && value > post && value > down) {
        if (tileindex2 > tileList.size()) {
          return false;
        }
        tileList[tileindex2 - 1].region.setBottomRight(QPoint(w, h));
        tileindex2++;
      }
    }
  }
  return tileindex == tileindex2;
}

} // namespace nim


