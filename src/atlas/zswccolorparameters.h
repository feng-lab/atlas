#pragma once

#include "zswcpack.h"
#include "znumericparameter.h"
#include "zoptionparameter.h"
#include "zcolormap.h"
#include <vector>

namespace nim {

class ZSwcColorParameters : public QObject
{
  Q_OBJECT

public:
  ZSwcColorParameters(QObject* parent = nullptr);

  void setData(ZSwcPack* swcPack);

  void initTopologyColor();

  void initTypeColor();

  void initSubclassTypeColor();

  glm::vec4 colorByType(const ZSwc::ConstSwcTreeNode& n);

  glm::vec4 colorByDirection(const ZSwc::ConstSwcTreeNode& n);

  glm::vec4 colorOfNode(const ZSwc::ConstSwcTreeNode& n);

  void adjustWidgets();

private:

public:
  ZStringIntOptionParameter colorMode;
  ZVec4Parameter swcTreeColor;
  std::vector<std::unique_ptr<ZVec4Parameter>> colorsForDifferentType;
  std::vector<std::unique_ptr<ZVec4Parameter>> colorsForSubclassType;
  std::map<int, size_t> subclassTypeColorMapper;
  std::vector<std::unique_ptr<ZVec4Parameter>> colorsForDifferentTopology;
  ZColorMapParameter colorMapBranchType;

private:
  ZSwcPack* m_swcPack = nullptr;
};

} // namespace nim
