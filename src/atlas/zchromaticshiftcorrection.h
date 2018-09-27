#pragma once

#include "zimgprocess.h"
#include <map>
#include <memory>

namespace nim {

class ZImg;

class ZImageTransform;

class ZImageCompositeTransform;

class ZChromaticShiftCorrection : public ZImgProcess
{
public:
  ZChromaticShiftCorrection(const ZImg& img, ZImg& correctedImg);

  // use this channel to do registration
  void setReferenceChannel(int ch)
  { m_referenceChannel = ch; }

  // correct this channel
  void setTargetChannel(int ch)
  { m_targetChannel = ch; }

  // some preprocess before registration
  // default is true
  void setRemoveBackground(bool v)
  { m_removeBackground = v; }

  // default is true
  void setRemoveHighForeground(bool v)
  { m_removeHighForeground = v; }

  // default is false
  void setBrightBackground(bool v)
  { m_brightBackground = v; }

  void setMethod(const QString& str)
  { m_method = str; }

  // registration methods
  void setMetric(const QString& str)
  { m_metric = str; }

  //
  void setTransform(const QString& str)
  { m_transform = str; }

  //
  void setOptimizer(const QString& str)
  { m_optimizer = str; }

  // default is true
  void setUseMultithreading(bool i)
  { m_useMultithreading = i; }

  // use multiscale registration if number of scale > 1, default is 1
  void setNumScales(int i)
  { m_numScales = i; }

protected:
  void doWork() override;

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
  void alignChannel(int fixedChannel, int movingChannel);

  template<typename ImagePixelType>
  void alignChannelWithPresetTransform(int movingChannel, const QString& presetName);

  template<typename ImagePixelType>
  void calcChannelInfs();

private:
  const ZImg& m_img;
  ZImg& m_correctedImg;

  int m_referenceChannel = -1;
  int m_targetChannel = -1;

  bool m_removeBackground = true;
  bool m_removeHighForeground = true;
  bool m_brightBackground = false;
  bool m_useMultithreading = true;
  int m_numScales = 1;

  std::vector<SectionInfo> m_channelInfos;
  double m_minValue;
  double m_maxValue;

  QString m_method{"Registration"};

  QString m_metric{"Normalized Cross-Correlation"};
  QString m_transform{"Translation"};
  QString m_optimizer{"LBFGS"};
};

} // namespace nim




