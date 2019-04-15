#pragma once

#include "zimgprocess.h"
#include <map>
#include <memory>

namespace nim {

class ZImg;

class ZImageTransform;

class ZImageCompositeTransform;

class ZSectionsRegistration : public ZImgProcess
{
Q_OBJECT
public:
  ZSectionsRegistration() = default;

  ZSectionsRegistration(const QStringList& imgFilenames, const QString& resultFilename, int fixedSliceIndex)
  {
    setInputOutput(imgFilenames, resultFilename, fixedSliceIndex);
  }

  void setInputOutput(const QStringList& imgFilenames, const QString& resultFilename, int fixedSliceIndex)
  {
    m_imgFilenames = imgFilenames;
    m_resultFilename = resultFilename;
    m_fixedSliceIndex = fixedSliceIndex;
  }

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

signals:

  void resultReady(QString path);

protected:
  void doWork() override;

  void read(const QJsonObject& json) override;

  void write(QJsonObject& json) const override;

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
  void alignSection(const ZImg& srcImg, int fixedImageIndex, int movingImageIndex,
                    double& cost, ZImageTransform*& transform);

  template<typename ImagePixelType>
  void transformSections(const std::map<size_t, std::unique_ptr<ZImageCompositeTransform>>& tfmmap, const ZImg& srcImg,
                         const QString& outImgFilename) const;

  template<typename ImagePixelType>
  void calcRefCh(const ZImg& srcImg);

  template<typename ImagePixelType>
  void calcSecInfs(const ZImg& srcImg);

private:
  QStringList m_imgFilenames;
  QString m_resultFilename;
  int m_fixedSliceIndex;

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

