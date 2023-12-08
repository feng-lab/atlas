#pragma once

#include "zimgprocess.h"
#include <map>
#include <memory>

namespace nim {

class ZImg;

class ZImageTransform;

class ZChromaticShiftCorrection : public ZImgProcess
{
  Q_OBJECT

public:
  ZChromaticShiftCorrection() = default;

  ZChromaticShiftCorrection(const QString& imgFilename, const QString& resultFilename)
  {
    setInputOutput(imgFilename, resultFilename);
  }

  void setInputOutput(const QString& imgFilename, const QString& resultFilename)
  {
    m_imgFilename = imgFilename;
    m_resultFilename = resultFilename;
  }

  // use this channel to do registration
  void setReferenceChannel(index_t ch)
  {
    m_referenceChannel = ch;
  }

  // correct this channel
  void setTargetChannel(index_t ch)
  {
    m_targetChannel = ch;
  }

  // some preprocess before registration
  // default is true
  void setRemoveBackground(bool v)
  {
    m_removeBackground = v;
  }

  // default is true
  void setRemoveHighForeground(bool v)
  {
    m_removeHighForeground = v;
  }

  // default is false
  void setBrightBackground(bool v)
  {
    m_brightBackground = v;
  }

  void setMethod(const QString& str)
  {
    m_method = str;
  }

  // registration methods
  void setMetric(const QString& str)
  {
    m_metric = str;
  }

  //
  void setTransform(const QString& str)
  {
    m_transform = str;
  }

  //
  void setOptimizer(const QString& str)
  {
    m_optimizer = str;
  }

  // default is true
  void setUseMultithreading(bool i)
  {
    m_useMultithreading = i;
  }

  // use multiscale registration if number of scale > 1, default is 1
  void setNumScales(size_t i)
  {
    m_numScales = i;
  }

Q_SIGNALS:
  void resultReady(QString path);

protected:
  void doWork() override;

  void read(const json::object& jo) override;

  void write(json::object& jo) const override;

private:
  struct SectionInfo
  {
    double min;
    double max;
    double mean;
    double median;
    double std;
  };

  template<typename ImagePixelType>
  void alignChannel(const ZImg& srcImg, size_t fixedChannel, size_t movingChannel);

  template<typename ImagePixelType>
  void alignChannelWithPresetTransform(const ZImg& srcImg, size_t movingChannel, const QString& presetName);

  template<typename ImagePixelType>
  void calcChannelInfs(const ZImg& srcImg);

private:
  QString m_imgFilename;
  QString m_resultFilename;
  index_t m_referenceChannel = -1;
  index_t m_targetChannel = -1;

  bool m_removeBackground = true;
  bool m_removeHighForeground = true;
  bool m_brightBackground = false;
  bool m_useMultithreading = true;
  size_t m_numScales = 1;

  std::vector<SectionInfo> m_channelInfos;
  double m_minValue{};
  double m_maxValue{};

  QString m_method{"Registration"};

  QString m_metric{"Normalized Cross-Correlation"};
  QString m_transform{"Translation"};
  QString m_optimizer{"LBFGS"};
};

} // namespace nim
