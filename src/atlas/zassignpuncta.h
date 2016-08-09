#pragma once

#include <QList>
#include <map>
#include "zimgprocess.h"
#include "zpuncta.h"
#include "zswc.h"

namespace nim {

class ZImg;

class ZAssignPuncta : public ZImgProcess
{
  using SwcTreeNode = ZSwc::Iterator;
public:
  ZAssignPuncta(const ZImg& img, size_t dendriteChannel, size_t t = 0);

  ~ZAssignPuncta();

  void setPuncta(const ZPuncta& puncta)
  { m_puncta = puncta; }

  void setSomaPuncta(const ZPuncta& somaPuncta)
  { m_somaPuncta = somaPuncta; }

  // default is 2.5um, valid range of puncta
  // puncta within this distance to branch are considered as belong to
  // this dendrite branch
  void setMaxDistToBranchInUm(double d)
  { m_maxDistToBranch = d; }

  // default is 1.0, Punctum is considered as ambiguous if it is within valid range
  // of many branches and (secondMinDistance < MinDistance * ambiguousFactor)
  void setAmbiguousFactor(double f)
  { m_ambiguousFactor = f; }

  void clearAllSwcTrees();

  void addSwcTree(ZSwc* tree);

  void addSwcTrees(const std::vector<ZSwc*>& trees);

  void addSwcTrees(std::vector<ZSwc>& trees);

  ZPuncta getPunctaOfTree(ZSwc* tree) const;

  ZPuncta getSomaPunctaOfTree(ZSwc* tree) const;

  ZPuncta getAmbiguousPuncta() const
  { return m_ambiguousPuncta; }

protected:
  virtual void doWork() override;

private:
  void separatePuncta();

  void separateSomaPuncta();

  double punctaTreeDist(const ZPunctum& punctum, ZSwc* tree, SwcTreeNode& nearestNode) const;

  std::vector<SwcTreeNode> nodesNearbyPuncta(const ZPunctum& punctum, ZSwc* tree) const;

  double punctaSomaDist(const ZPunctum& punctum, ZSwc* tree) const;

  double pointFrustumConeDist(double x, double y, double z, const SwcTreeNode& tn, const SwcTreeNode& ptn) const;

  SwcTreeNode intensityWeightedNearestNode(double x, double y, double z,
                                           const std::vector<SwcTreeNode>& nodes, bool& isAmbiguous);

  SwcTreeNode nearestNode(double x, double y, double z, const std::vector<SwcTreeNode>& nodes);

private:
  // input
  const ZImg& m_img;
  size_t m_dendriteChannel;
  size_t m_t;
  ZPuncta m_puncta;
  ZPuncta m_somaPuncta;
  // image info
  double m_maxDistToBranch; // in um

  double m_ambiguousFactor;

  // input trees and results
  ZPuncta m_ambiguousPuncta;
  std::map<ZSwc*, ZPuncta> m_swcTreeToPuncta;
  std::map<ZSwc*, ZPuncta> m_swcTreeToSomaPuncta;

  const int m_somaType = 1;
};

} // namespace nim

