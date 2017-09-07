#pragma once

#include "zimgprocess.h"
#include "zeigenutils.h"
#include "zpuncta.h"
#include <QList>

namespace nim {

class ZImg;

class ZSwc;

class ZPunctaDetection : public ZImgProcess
{
public:
  ZPunctaDetection(const ZImg& img, size_t punctaChannel, size_t t = 0);

  // if set, result will be saved to these files
  void setResultPunctaFilename(const QString& fn)
  { m_detectedPunctaFileName = fn; }

  void setResultSomaPunctaFilename(const QString& fn)
  { m_detectedSomaPunctaFileName = fn; }

  // if not set, auto threshold will be used
  void setPunctaThreshold(int thre)
  { m_punctaThreshold = thre; }

  // miminum voxel number to goto split step, default is 20 voxels
  void setSplitThreshold(int thre)
  { m_splitSizeThreshold = thre; }

  // confidence region of gaussian, used to estimate punctum radius, default is 0.95
  void setConfidenceRegionForRadiusEstimate(double c)
  { m_confRadius = c; }

  // confidence region of gaussian, used to get overlap area of error ellipse, default is 0.8
  void setConfidenceRegionForOverlapArea(double c)
  { m_confOverlapArea = c; }

  // overlap threshold to merge two gaussian, default is 0.8
  void setOverlapRateThreshold(double t)
  { m_overlapRateThreshold = t; }

  // for watershed
  void setSeedSizeThreshold(int i)
  { m_seedSizeThreshold = i; }

  // default is true
  void setUseMultithreading(bool v)
  { m_useMultithreading = v; }

  // optional, if set, soma will be detected and puncta in soma area will be
  // detected differently.
  void setDendriteChannel(int c)
  { m_dendriteChannel = c; }

  // for soma detection
  // default use 2.6um
  void setMaxDendriteTubeRadiusInUm(double mtr)
  { m_maxDendriteTubeRadius = mtr; }

  void setDendriteThreshold(double tt)
  { m_dendriteThreshold = tt; }

  // assign puncta to each swc tree if set
  void clearAllSwcTrees()
  { m_swcTrees.clear(); }

  void setSwcTrees(const std::vector<ZSwc*>& trees, const QStringList& treePaths);

  void setSwcTrees(std::vector<ZSwc>& trees, const QStringList& treePaths);

  // default is 2.5um, valid range of puncta
  // puncta within this distance to branch are considered as belong to
  // this dendrite branch
  void setMaxDistToBranchInUm(double d)
  { m_maxDistToBranch = d; }

  // default is 1.0, Punctum is considered as ambiguous if it is within valid range
  // of many branches and (secondMinDistance < MinDistance * ambiguousFactor)
  void setAmbiguousFactor(double f)
  { m_ambiguousFactor = f; }

  static double getOverlapRateOfTwoErrorEllipse(Eigen::RowVectorXd m1, Eigen::MatrixXd cov1, Eigen::RowVectorXd m2,
                                                Eigen::MatrixXd cov2, double conf = 0.8);

protected:
  void doWork() override;

private:
  // all works are done here, detect from img with thre and put result into resList
  // img will be cleared after using
  void detectImpl(ZImg& img, int thre, ZPuncta& resList, ZPuncta& filteredList,
                  const Eigen::RowVectorXi& minLoc, double weight, double baseWeight);

  // detect puncta in soma area, save result in m_detectedSomaPuncta, return soma voxels
  // input preprcocessedImage contains only foreground voxels
  template<typename Image3DType>
  Eigen::MatrixXi detectSomaPuncta(const Image3DType* preprocessedImage);

  // remove all detected punctum
  void cleanup();

  QString getPunctaOutputFilename(const QString& swcPath);

  QString getSomaPunctaOutputFilename(const QString& swcPath);

  QString getAmbiguousPunctaOutputFileName();

  QString getFilteredPunctaFilename();

  QString getFilteredSomaPunctaFilename();

  // simple method, remove all tube based on its maximum radius (default use 2.6um)
  // typical soma diameter would be 10-15um
  void detectSomaMask(Eigen::MatrixXi& small, Eigen::MatrixXi& big);

  int cropOutSomaImg(ZImg& img, ZImg& somaImg, Eigen::RowVectorXi& minLoc);

  std::vector<Eigen::MatrixXi> watershedSplit(const ZImg& img) const;

  void getVoxelRange(const Eigen::MatrixXi& voxelLocations, Eigen::RowVectorXi& minLoc, Eigen::RowVectorXi& size);

  Eigen::VectorXd getVoxelIntensities(const Eigen::MatrixXi& voxelLocations, const ZImg& img, size_t c, size_t t);

  // crop with minLoc and size, then set any voxel other than voxels in voxelLocations as zero
  // both img and res are uint8_t type
  ZImg cropZImg(const Eigen::MatrixXi& voxelLocations, const nim::ZImg& img, size_t c, size_t t,
                const Eigen::RowVectorXi& minLoc, const Eigen::RowVectorXi& size);

  ZImg cropZImg(const nim::ZImg& img, size_t c, size_t t,
                const Eigen::RowVectorXi& minLoc, const Eigen::RowVectorXi& size);

  size_t getNumCenters(const Eigen::MatrixXi& voxelLocations, const Eigen::VectorXd& voxelIntensities,
                       const ZImg& locmax, double saturatedIntensity, const Eigen::RowVectorXi& minLoc);

  // split voxels use vbgmm and save result puncta into detectedPunctaList
  // voxelLocs is coordinates of voxels in stack, voxelIntens is intensities of these voxels.
  // minLoc is the start Location of stack in case this stack is a cropped region.
  static void vbgmmSplit(const Eigen::MatrixXi& voxelLocs, const Eigen::VectorXd& voxelIntens,
                         size_t numCenter, const ZImg& img, ZPuncta& detectedPunctaList,
                         double confRadius = .95, double confOverlapArea = .8, double overlapRateThreshold = .8,
                         Eigen::RowVectorXi minLoc = Eigen::RowVectorXi::Zero(3));

private:
  const ZImg& m_img;
  size_t m_punctaChannel;
  size_t m_t;

  // parameters
  int m_punctaThreshold = -1;
  int m_splitSizeThreshold = 20;
  double m_confRadius = 0.95;
  double m_confOverlapArea = 0.8;
  double m_overlapRateThreshold = 0.8;
  int m_seedSizeThreshold = 6;
  bool m_useMultithreading = true;

  // parameters for soma detection
  int m_dendriteChannel = -1;
  double m_maxDendriteTubeRadius = 2.6;  // in um
  double m_dendriteThreshold = 100;

  // parameters for assign puncta to swc tree
  double m_maxDistToBranch = 2.5; // in um
  double m_ambiguousFactor = 1.0;

  ZPuncta m_detectedSomaPuncta;
  ZPuncta m_detectedPuncta;
  ZPuncta m_filteredSomaPuncta;
  ZPuncta m_filteredPuncta;
  std::vector<ZSwc*> m_swcTrees;
  QStringList m_swcPaths;

  QString m_detectedPunctaFileName;
  QString m_detectedSomaPunctaFileName;
};

} // namespace nim

