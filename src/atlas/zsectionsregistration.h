#pragma once

#include "zimgprocess.h"
#include <map>

namespace nim {

class ZImg;

class ZImageTransform;

class ZImageCompositeTransform;

class ZSectionsRegistration : public ZImgProcess
{
public:
  ZSectionsRegistration(const ZImg& img, int fixedSliceIndex, ZImg& registeredImg);

  // use this channel to do registration, if not set or set to -1, the channel with strongest image signal
  // will be used.
  void setReferenceChannel(int ch)
  { m_referenceChannel = ch; }

  // some preprocess before registration
  // default is true
  void setRemoveBackground(bool v)
  { m_removeBackground = v; }

  // default is true
  void setRemoveHighForeground(bool v)
  { m_removeHighForeground = v; }

  // default is false
  void setAllowFlip(bool v)
  { m_allowFlip = v; }

  // default is false
  void setBrightBackground(bool v)
  { m_brightBackground = v; }

  // registration methods
  //
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

  // registrate between s, s+1, ..., s+i, s-1, ..., s-i
  void setNumNeighbors(int i)
  { m_numNeighbors = i; }

protected:
  virtual void doWork() override;

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
  void alignSection(int fixedImageIndex, int movingImageIndex, double& cost, ZImageTransform*& transform);

  template<typename ImagePixelType>
  void transformSections(const std::map<size_t, std::unique_ptr<ZImageCompositeTransform>>& tfmmap, const ZImg& inImg,
                         ZImg& outImg) const;

  template<typename ImagePixelType>
  void alignSection(int fixedImageIndex, int movingImageIndex);

  template<typename ImagePixelType>
  void calcRefCh();

  template<typename ImagePixelType>
  void calcSecInfs();

private:
  const ZImg& m_img;
  int m_fixedSliceIndex;

  ZImg& m_registeredImg;

  int m_referenceChannel = -1;

  bool m_removeBackground = true;
  bool m_removeHighForeground = true;
  bool m_allowFlip = false;
  bool m_brightBackground = false;
  bool m_useMultithreading = true;
  int m_numScales = 1;
  int m_numNeighbors = 1;

  std::vector<SectionInfo> m_sectionInfos;
  double m_minValue;
  double m_maxValue;

  QString m_metric{"Log Absolute Differences"};
  QString m_transform{"Rigid"};
  QString m_optimizer{"LBFGS"};
};

} // namespace nim

