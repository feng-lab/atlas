#pragma once

#include "zimgprocess.h"
#include "zimg.h"
#include "zimginfo.h"
#include "zimgregion.h"
#include "zimgmerge.h"
#include "zstringutils.h"
#include <QList>
#include <QRect>

namespace nim {

class ZStitchImage : public ZImgProcess
{
public:
  ZStitchImage();

  void setInputFilenames(const QStringList& fns, size_t scene = 0)
  {
    m_inputFilenames = fns;
    std::sort(m_inputFilenames.begin(), m_inputFilenames.end(), naturalSortLessThan);
    m_scene = scene;
  }

  // if set, result will be saved to these files
  void setResultFilename(const QString& fn)
  { m_resFileName = fn; }

  // default use all channels (empty input)
  void setUseChannels(const std::vector<size_t>& chs = std::vector<size_t>())
  { m_channelsToUse = chs; }

  // default apply to no channel
  void setRemoveBackgroundForChannels(const std::vector<size_t>& chs = std::vector<size_t>())
  { m_channelsToRemoveBackground = chs; }

  // for stitching two sets of images with one common channel and same tile cofigurations
  void set2ndInput(const QStringList& fns, size_t scene, const std::vector<size_t>& useChs,
                   const std::vector<size_t>& chsForBackgroundRemove,
                   size_t commonChannelOfInput, size_t commonChannelof2ndInput)
  {
    m_2ndInputFilenames = fns;
    std::sort(m_2ndInputFilenames.begin(), m_2ndInputFilenames.end(), naturalSortLessThan);
    m_2ndScene = scene;
    m_2ndChannelsToUse = useChs;
    m_2ndChannelsToRemoveBackground = chsForBackgroundRemove;
    m_commonChannelOfInput = commonChannelOfInput;
    m_commonChannelOf2ndInput = commonChannelof2ndInput;
  }

  // default Max
  void setMergeMode(ImgMergeMode mode)
  { m_mergeMode = mode; }

  void setDownsampleBeforeStitching(size_t blockWidth, size_t blockHeight, size_t blockDepth,
                                    ImgMergeMode mode = ImgMergeMode::Mean)
  {
    m_downsampleBlockWidth = blockWidth;
    m_downsampleBlockHeight = blockHeight;
    m_downsampleBlockDepth = blockDepth;
    m_downsampleMergeMode = mode;
  }

  // use coarse-to-fine method to reduce memory usage
  // coarse image is created by averaging pixels in block of size (intvX + 1, intvY + 1, intvZ + 1)
  void setStartResolution(size_t intvX, size_t intvY, size_t intvZ)
  {
    m_startResolutionIntvX = intvX;
    m_startResolutionIntvY = intvY;
    m_startResolutionIntvZ = intvZ;
  }

  // only works with grid or conn text file
  void setConcatenateOnly()
  {
    m_concatenateOnly = true;
  }

  // default 0.15
  void setMaxOverlapRate(double maxOverlapRate)
  { m_maxOverlapRate = maxOverlapRate; }

  // set Tile configuration to be one of grid, conn text file, blind, czi restitching

  // matrix with the grid configuration of tiles, positive number represents tile index
  // [0 0 1 2 0; 0 0 3 0 0]: tile 2 is on the right side of tile 1, tile 3 is at bottom side of tile 1
  // the maximum index in the grid matrix must match the number of inputs
  void setTileGrid(const ZImg& tileGrid)
  {
    unsetTileConfiguration();
    m_tileGrid = tileGrid;
  }

  // derive m_tileGrid from Zeiss tile selection image
  void setConnTileImage(const QString& fn);

  // read m_tileGrid from text file that contains a matrix
  void setTileGridFromMatrixFile(const QString& file);

  // derive m_tileGrid from a matrix
  void setTileGridFromLayout(size_t numRows, size_t numCols);

  void setConnInfoFromConnTextFile(const QString& file)
  {
    unsetTileConfiguration();
    m_connTextFile = file;
  }

  void setRestitch()
  {
    unsetTileConfiguration();
    m_restitch = true;
  }

  void setBlindStitching()
  {
    unsetTileConfiguration();
  }

protected:
  void doWork() override;

  void read(const QJsonObject& json) override;

  void write(QJsonObject& json) const override;

private:
  void getTileMatrixFromConnImage(ZImg& img);

  void unsetTileConfiguration();

  void doRestitch();

private:
  QStringList m_inputFilenames;
  size_t m_scene = 0;
  QString m_resFileName;

  // parameters
  std::vector<size_t> m_channelsToUse;  //empty means all channel
  std::vector<size_t> m_channelsToRemoveBackground;
  ImgMergeMode m_mergeMode = ImgMergeMode::Max;
  size_t m_downsampleBlockWidth = 1;
  size_t m_downsampleBlockHeight = 1;
  size_t m_downsampleBlockDepth = 1;
  ImgMergeMode m_downsampleMergeMode = ImgMergeMode::Mean;
  bool m_concatenateOnly = false;
  size_t m_startResolutionIntvX = 1;  // start from image downsampled by (m_startResolutionIntvX+1)
  size_t m_startResolutionIntvY = 1;  // start from image downsampled by (m_startResolutionIntvY+1)
  size_t m_startResolutionIntvZ = 1;  // start from image downsampled by (m_startResolutionIntvZ+1)
  double m_maxOverlapRate = 0.15;

  // one of the configuration will be valid:
  ZImg m_tileGrid;  // a 2d or 3d img contains the tile grid
  QString m_connTextFile;
  bool m_restitch = false; // only works for czi now

  // another set of stacks that have common channel and same tile configuraton with the main inputs
  QStringList m_2ndInputFilenames;
  size_t m_2ndScene;
  std::vector<size_t> m_2ndChannelsToUse;  //empty means all channel
  std::vector<size_t> m_2ndChannelsToRemoveBackground;
  size_t m_commonChannelOfInput = 0;
  size_t m_commonChannelOf2ndInput = 0;
};

} // namespace nim

