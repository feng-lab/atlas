#pragma once

#include "zimgprocess.h"
#include "zpuncta.h"
#include "zswc.h"
#include "zimginfo.h"
#include <QList>
#include <map>

namespace nim {

class ZImg;

class ZAssignPuncta : public ZImgProcess
{
public:
  ZAssignPuncta(const ZImg& img, size_t dendriteChannel, size_t t = 0);

  // for big image, minValue and maxValue are used to convert image into uint8_t if it is not
  ZAssignPuncta(QString  filename, double minValue, double maxValue,
                size_t dendriteChannel = 0, size_t t = 0, size_t scene = 0);

  // only image resolution in imgInfo is used
  ZAssignPuncta(QString filename, const ZImgInfo& imgInfo, double minValue, double maxValue,
                size_t dendriteChannel = 0, size_t t = 0, size_t scene = 0);

  ~ZAssignPuncta() override;

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

  void addSwcTree(const ZSwc* tree);

  void addSwcTrees(const std::vector<ZSwc>& trees);

  ZPuncta getPunctaOfTree(const ZSwc* tree) const;

  ZPuncta getSomaPunctaOfTree(const ZSwc* tree) const;

  [[nodiscard]] ZPuncta getAmbiguousPuncta() const
  { return m_ambiguousPuncta; }

protected:
  void doWork() override;

  void read(const QJsonObject&) override
  {}

  void write(QJsonObject&) const override
  {}

private:
  void separatePuncta();

  void separateSomaPuncta();

  double punctaTreeDist(const ZPunctum& punctum, const ZSwc* tree, ZSwc::ConstSwcTreeNode& nearestNode) const;

  std::vector<ZSwc::ConstSwcTreeNode> nodesNearbyPuncta(const ZPunctum& punctum, const ZSwc* tree) const;

  double punctaSomaDist(const ZPunctum& punctum, const ZSwc* tree) const;

  [[nodiscard]] double pointFrustumConeDist(double x, double y, double z, const ZSwc::ConstSwcTreeNode& tn,
                                            const ZSwc::ConstSwcTreeNode& ptn) const;

  ZSwc::ConstSwcTreeNode intensityWeightedNearestNode(double x, double y, double z,
                                                      const std::vector<ZSwc::ConstSwcTreeNode>& nodes,
                                                      bool& isAmbiguous);

  ZSwc::ConstSwcTreeNode nearestNode(double x, double y, double z, const std::vector<ZSwc::ConstSwcTreeNode>& nodes);

private:
  // input
  const ZImg* m_img = nullptr;
  QString m_filename;
  ZImgInfo m_imgInfo;
  double m_minValue{};
  double m_maxValue{};
  size_t m_dendriteChannel;
  size_t m_t;
  size_t m_scene{};
  ZPuncta m_puncta;
  ZPuncta m_somaPuncta;
  // image info
  double m_maxDistToBranch = 2.5; // in um

  double m_ambiguousFactor = 1.0;

  // input trees and results
  ZPuncta m_ambiguousPuncta;
  std::map<const ZSwc*, ZPuncta> m_swcTreeToPuncta;
  std::map<const ZSwc*, ZPuncta> m_swcTreeToSomaPuncta;

  const int m_somaType = 1;
};

} // namespace nim

