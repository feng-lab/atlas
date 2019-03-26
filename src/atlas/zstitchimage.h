#pragma once

#include "zimgprocess.h"
#include "zimg.h"
#include "zimginfo.h"
#include "zimgregion.h"
#include "zimgmerge.h"
#include "zstringutils.h"
#include <QList>
#include <QImage>

namespace nim {

class ZTile
{
public:
  ZTile(int index_, QPoint topleft, QPoint bottomright);

  bool bIsSelected = true;
  int index;
  QRect region;
};

class ZStitchImage : public ZImgProcess
{
public:
  ZStitchImage();

  void setInputFilenames(const QStringList& fns)
  {
    m_inputStack1Filenames = fns;
    std::sort(m_inputStack1Filenames.begin(), m_inputStack1Filenames.end(), naturalSortLessThan);
  }

  // if set, result will be saved to these files
  void setResultFilename(const QString& fn)
  { m_resFileName = fn; }

  // default use all channels (empty input)
  void setUseChannels(const std::vector<size_t>& chs)
  { m_channelsToUse = chs; }

  void setUseAllChannels()
  { m_channelsToUse.clear(); }

  // default Max
  void setMergeMode(ZImgMerge::Mode mode)
  { m_mergeMode = mode; }

  // default 0.1
  void setMaxOverlapRate(double maxOverlapRate)
  { m_maxOverlapRate = maxOverlapRate; }

  //
  void setConnTileImage(const QString& fn);

protected:
  void doWork() override;

private:
  bool getTileMatrix(ZImg& img, std::vector<std::vector<int>>& tileMatrix, QList<ZTile>& tileList);

private:
  QStringList m_inputStack1Filenames;
  QString m_resFileName;

  // parameters
  std::vector<size_t> m_channelsToUse;  //empty means all channel
  ZImgMerge::Mode m_mergeMode = ZImgMerge::Mode::Max;
  double m_maxOverlapRate = 0.1;
  QString m_tileSelectionImageFilename;
  QImage m_tileImage;
  std::vector<std::vector<int>> m_tileMatrix;
  QList<ZTile> m_tileList;
  // int m_nSel;
};

} // namespace nim

