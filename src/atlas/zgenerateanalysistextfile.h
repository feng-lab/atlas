#pragma once

#include "zimginterface.h"
#include <map>
#include <vector>
#include <QDir>
#include "zpuncta.h"
#include "zswc.h"

namespace nim {

struct ZAnalysisTextFileInput
{
  ZAnalysisTextFileInput()
    : voxelSizeX(-1), voxelSizeY(-1), voxelSizeZ(-1)
    , dendriteChannel(-1), axonChannel(-1), maxDistToBranch(2.5), bluenessExtend(2.5)
    , doPyramidalFunctionalSeparation(false), doPyramidalSubclassSeparation(false)
  {}

  QString imgFilename;
  QString swcFilename;
  QString punctaFilename;

  // image info
  double voxelSizeX;   // in um
  double voxelSizeY;   // in um
  double voxelSizeZ;   // in um

  int dendriteChannel;
  int axonChannel;
  double maxDistToBranch; // in um
  double bluenessExtend; // in um

  QString outputFolder;

  bool doPyramidalFunctionalSeparation;
  bool doPyramidalSubclassSeparation;
  QString somaPunctaFilename;
};

class ZGenerateAnalysisTextFile
{
  typedef ZSwc::Iterator SwcTreeNode;
  typedef ZSwc::ConstIterator ConstSwcTreeNode;
public:
  ZGenerateAnalysisTextFile();

  // -----  first way to call ---------

  void generate(const ZAnalysisTextFileInput input);

  // -----  second way to call ---------

  // must set if img file don't contain resolution
  void setVoxelSizeXInUm(double x)
  { m_input.voxelSizeX = x; }

  void setVoxelSizeYInUm(double y)
  { m_input.voxelSizeY = y; }

  void setVoxelSizeZInUm(double z)
  { m_input.voxelSizeZ = z; }

  void setVoxelSizeInUm(double x, double y, double z)
  {
    m_input.voxelSizeX = x;
    m_input.voxelSizeY = y;
    m_input.voxelSizeZ = z;
  }

  // must set
  void setDendriteChannel(size_t c)
  { m_input.dendriteChannel = c; }

  // optional, if set, will calculate blueness
  void setAxonChannel(size_t c)
  { m_input.axonChannel = c; }

  // default is 2.5um, valid range of puncta
  // puncta within this distance to branch are considered as belong to
  // this dendrite branch
  void setMaxDistToBranchInUm(double d)
  { m_input.maxDistToBranch = d; }

  // extend branch diameter to calculate blueness, default is 2.5um
  void setBluenessExtend(double d)
  { m_input.bluenessExtend = d; }

  // optional, default create a subfolder with same name as swc in swc file folder
  void setOutputFolder(const QString& folder)
  { m_input.outputFolder = folder.trimmed(); }

  // also do pyramidal functional branch analysis, default is false, swc will be converted to pyramidal
  void setDoPyramidalFunctionalSeparation(bool v)
  { m_input.doPyramidalFunctionalSeparation = v; }

  // also do pyramidal subclass branch analysis, default is false, swc will be converted to pyramidal
  void setDoPyramidalSubclassSeparation(bool v)
  { m_input.doPyramidalSubclassSeparation = v; }

  void generate(const QString& imgFilename, const QString& swcFilename, const QString& punctaFilename);

  // -----  third way to call ---------

  // parse input file
  void generate(const QString& worklistFile);

protected:
  void generate();

  void checkFileExist(const QString& filename) const;

  // if axon channel exist, set tree node feature to average axon intensity, otherwise set to 0
  void getAxonFeature(const ZSwc& tree, std::map<ConstSwcTreeNode, double>& nodeToBlueness) const;

  void getLayerFeature(const ZSwc& tree, const ZSwc& layerTree, std::map<ConstSwcTreeNode, size_t>& nodeToLayer) const;

  void getSubclassFeature(const ZSwc& tree, const ZSwc& subclassTree,
                          std::map<ConstSwcTreeNode, size_t>& nodeToSubclass) const;

  void writeFeatureSwc(const ZSwc& tree, const std::map<ConstSwcTreeNode, double>& nodeToFeature,
                       const QString& outSwcName) const;

  // return point to swc segment distance in um
  double pointFrustumConeDist(double x, double y, double z,
                              const ConstSwcTreeNode& start, const ConstSwcTreeNode& end,
                              double* frac = nullptr) const;

  double pointSphereDist(double x, double y, double z, const ConstSwcTreeNode& tn) const;

  double punctaFrustumConeDist(const ZPunctum& punctum,
                               const ConstSwcTreeNode& start, const ConstSwcTreeNode& end,
                               double* frac = nullptr) const;

  double treeNodeDist(const ConstSwcTreeNode& tn, const ConstSwcTreeNode& ptn) const;

  bool inputSwcIsPyramidal() const;

  void mergeSoma(ZSwc& tree, std::map<ConstSwcTreeNode, double>& nodeToBlueness,
                 std::map<ConstSwcTreeNode, size_t>& nodeToLayer) const;

  void removeSmallLeafBranch(ZSwc& tree, int numNodeThre, double lengthThre) const; // lengthThre in um

  // label branch and calculate some properties for each tree node, return number of branches
  // branch id start from 1, soma doesn't count as branch
  struct Branch
  {
    Branch()
      : id(0), length(0.0)
    {}

    size_t id;
    double length;  // in um
    std::vector<ConstSwcTreeNode> nodes;
  };

  size_t labelBranch(const ZSwc& tree,
                     std::map<ConstSwcTreeNode, size_t>& nodeToBranchId,
                     std::map<size_t, size_t>& branchIdToParentBranchId,
                     std::map<ConstSwcTreeNode, double>& nodeDistToParent,
                     std::map<ConstSwcTreeNode, double>& nodeDistToBranchStart,
                     std::map<ConstSwcTreeNode, double>& nodeDistToSoma,
                     std::map<ConstSwcTreeNode, int>& nodeTopologyType,
                     std::vector<Branch>& branches) const;

  // punctum belongs to returned tree node and its parent
  ConstSwcTreeNode getNodeSegOfPunctum(const ZSwc& tree, const ZPunctum& punctum, size_t numBranches,
                                       const std::map<ConstSwcTreeNode, size_t>& nodeToBranchId) const;

  ConstSwcTreeNode intensityWeightedNearestNode(double x, double y, double z,
                                                const std::vector<ConstSwcTreeNode>& nodes) const;

  ConstSwcTreeNode nearestNode(double x, double y, double z, const std::vector<ConstSwcTreeNode>& nodes) const;

  // go to subfolder, create if neccessary
  QDir getSubDir(const QString& subFoldername) const;

  void generateAnalysisFiles(const ZSwc& tree, const std::map<ConstSwcTreeNode, double>& nodeToBlueness,
                             const std::map<ConstSwcTreeNode, size_t>& nodeToLayer,
                             const std::map<ConstSwcTreeNode, size_t>& nodeToSubclass) const;

private:
  ZAnalysisTextFileInput m_input;

  QString m_processedSwcFilename;
  QString m_layerSwcFilename;
  QString m_bluenessSwcFilename;
  QString m_subclassSwcFilename;
};

} // namespace nim

