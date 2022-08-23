#include "zswccolorparameters.h"

namespace nim {

ZSwcColorParameters::ZSwcColorParameters(QObject* parent)
  : QObject(parent)
  , colorMode("Color Mode")
  , swcTreeColor("Color", glm::vec4(1, 0, 0, 1))
  , colorMapBranchType("Branch Type Color Map")
{
  initTopologyColor();
  initTypeColor();
  initSubclassTypeColor();

  colorMode.addOptions("Single Color", "Branch Type", "Topology", "Colormap Branch Type"
                       // "Subclass"
  );

  colorMode.select("Branch Type");

  connect(&colorMode, &ZStringIntOptionParameter::valueChanged, this, &ZSwcColorParameters::adjustWidgets);

  swcTreeColor.setStyle("COLOR");

  adjustWidgets();
}

void ZSwcColorParameters::setData(ZSwcPack* swcPack)
{
  m_swcPack = swcPack;
  if (m_swcPack) {
    // get min max of type for colormap
    colorMapBranchType.blockSignals(true);
    if (m_swcPack->allNodeType().empty()) {
      colorMapBranchType.get().reset();
    } else {
      colorMapBranchType.get().reset(m_swcPack->allNodeType().begin(),
                                     m_swcPack->allNodeType().end(),
                                     glm::col4(0, 0, 255, 255),
                                     glm::col4(255, 0, 0, 255));
    }
    colorMapBranchType.blockSignals(false);
  }

  adjustWidgets();
}

void ZSwcColorParameters::initTopologyColor()
{
  // topology colors (root, branch point, leaf, others)
  colorsForDifferentTopology.emplace_back(
    std::make_unique<ZVec4Parameter>("Root Color", glm::vec4(0 / 255.f, 0 / 255.f, 255 / 255.f, 1.f)));
  colorsForDifferentTopology.emplace_back(
    std::make_unique<ZVec4Parameter>("Branch Point Color", glm::vec4(0 / 255.f, 255 / 255.f, 0 / 255.f, 1.f)));
  colorsForDifferentTopology.emplace_back(
    std::make_unique<ZVec4Parameter>("Leaf Color", glm::vec4(255 / 255.f, 255 / 255.f, 0 / 255.f, 1.f)));
  colorsForDifferentTopology.emplace_back(
    std::make_unique<ZVec4Parameter>("Other", glm::vec4(255 / 255.f, 0 / 255.f, 0 / 255.f, 1.f)));
  for (const auto& color : colorsForDifferentTopology) {
    color->setStyle("COLOR");
  }
}

void ZSwcColorParameters::initTypeColor()
{
  // type colors
  int index = 0;
  QString name = QString("Type %1 Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(255 / 255.f, 255 / 255.f, 255 / 255.f, 1.f))); // white
  // 1
  name = QString("Type %1 (Soma) Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(20 / 255.f, 20 / 255.f, 20 / 255.f, 1.f))); // black
  // 2
  name = QString("Type %1 (Axon) Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(200 / 255.f, 20 / 255.f, 0 / 255.f, 1.f))); // red
  // 3
  name = QString("Type %1 (Basal Dendrite) Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0 / 255.f, 20 / 255.f, 200 / 255.f, 1.f))); // blue
  // 4
  name = QString("Type %1 (Apical Dendrite) Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(200 / 255.f, 0 / 255.f, 200 / 255.f, 1.f))); // purple
  // 5
  name = QString("Type %1 (Main Trunk) Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0 / 255.f, 200 / 255.f, 200 / 255.f, 1.f))); // cyan
  // 6
  name = QString("Type %1 (Basal Intermediate) Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(220 / 255.f, 200 / 255.f, 0 / 255.f, 1.f))); // yellow
  // 7
  name = QString("Type %1 (Basal Terminal) Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0 / 255.f, 200 / 255.f, 20 / 255.f, 1.f))); // green
  // 8
  name = QString("Type %1 (Apical Oblique Intermediate) Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(188 / 255.f, 94 / 255.f, 37 / 255.f, 1.f))); // coffee
  // 9
  name = QString("Type %1 (Apical Oblique Terminal) Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(180 / 255.f, 200 / 255.f, 120 / 255.f, 1.f))); // asparagus
  // 10
  name = QString("Type %1 (Apical Tuft) Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(250 / 255.f, 100 / 255.f, 120 / 255.f, 1.f))); // salmon
  // 11
  name = QString("Type %1 Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(120 / 255.f, 200 / 255.f, 200 / 255.f, 1.f))); // ice
  // 12
  name = QString("Type %1 Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(100 / 255.f, 120 / 255.f, 200 / 255.f, 1.f))); // orchid
  // 13
  name = QString("Type %1 Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(255 / 255.f, 128 / 255.f, 168 / 255.f, 1.f)));
  // 14
  name = QString("Type %1 Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(128 / 255.f, 255 / 255.f, 168 / 255.f, 1.f)));
  // 15
  name = QString("Type %1 Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(128 / 255.f, 168 / 255.f, 255 / 255.f, 1.f)));
  // 16
  name = QString("Type %1 Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(128 / 255.f, 255 / 255.f, 168 / 255.f, 1.f)));
  // 17
  name = QString("Type %1 Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(255 / 255.f, 168 / 255.f, 128 / 255.f, 1.f)));
  // 18
  name = QString("Type %1 Color").arg(index++);
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(168 / 255.f, 128 / 255.f, 255 / 255.f, 1.f)));
  // 19
  name = QString("Undefined Type Color");
  colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0xcc / 255.f, 0xcc / 255.f, 0xcc / 255.f, 1.f)));

  for (const auto& color : colorsForDifferentType) {
    color->setStyle("COLOR");
  }
}

void ZSwcColorParameters::initSubclassTypeColor()
{
  // subclass type color
  QString name = QString("Soma Color");
  subclassTypeColorMapper[1] = colorsForSubclassType.size();
  colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0 / 255.f, 0 / 255.f, 0 / 255.f, 1.f)));
  name = QString("Main Trunk Color");
  subclassTypeColorMapper[5] = colorsForSubclassType.size();
  colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0 / 255.f, 0 / 255.f, 0 / 255.f, 1.f)));
  name = QString("Basal Intermediate Color");
  subclassTypeColorMapper[6] = colorsForSubclassType.size();
  colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0x33 / 255.f, 0xcc / 255.f, 0xff / 255.f, 1.f)));
  name = QString("Basal Terminal Color");
  subclassTypeColorMapper[7] = colorsForSubclassType.size();
  colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0x33 / 255.f, 0x66 / 255.f, 0xcc / 255.f, 1.f)));
  name = QString("Apical Oblique Intermediate Color");
  subclassTypeColorMapper[8] = colorsForSubclassType.size();
  colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0xff / 255.f, 0xff / 255.f, 0 / 255.f, 1.f)));
  name = QString("Apical Oblique Terminal Color");
  subclassTypeColorMapper[9] = colorsForSubclassType.size();
  colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0xcc / 255.f, 0x33 / 255.f, 0x66 / 255.f, 1.f)));
  name = QString("Apical Tuft Color");
  subclassTypeColorMapper[10] = colorsForSubclassType.size();
  colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0 / 255.f, 0x99 / 255.f, 0 / 255.f, 1.f)));
  name = QString("Other Undefined class Color");
  colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0xcc / 255.f, 0xcc / 255.f, 0xcc / 255.f, 1.f)));
  for (const auto& color : colorsForSubclassType) {
    color->setStyle("COLOR");
  }
}

glm::vec4 ZSwcColorParameters::colorByType(const ZSwc::ConstSwcTreeNode& n)
{
  if (colorMode.isSelected("Branch Type")) {
    if (static_cast<size_t>(n->type) + 1 < colorsForDifferentType.size()) {
      return colorsForDifferentType[n->type]->get();
    } else {
      return colorsForDifferentType[colorsForDifferentType.size() - 1]->get();
    }
  } else if (colorMode.isSelected("Subclass")) {
    if (subclassTypeColorMapper.find(n->type) != subclassTypeColorMapper.end()) {
      return colorsForSubclassType[subclassTypeColorMapper[n->type]]->get();
    } else {
      return colorsForSubclassType[colorsForSubclassType.size() - 1]->get();
    }
  } else /*if (colorMode.get() == "ColorMap Branch Type")*/ {
    return colorMapBranchType.get().mappedFColor(n->type);
  }
}

glm::vec4 ZSwcColorParameters::colorByDirection(const ZSwc::ConstSwcTreeNode& /*n*/)
{
  return glm::vec4(0);
}

glm::vec4 ZSwcColorParameters::colorOfNode(const ZSwc::ConstSwcTreeNode& n)
{
  if (colorMode.isSelected("Branch Type") || colorMode.isSelected("Colormap Branch Type") ||
      colorMode.isSelected("Subclass")) {
    return colorByType(n);
  } else if (colorMode.isSelected("Single Color")) {
    return swcTreeColor.get();
  } else if (colorMode.isSelected("Topology")) {
    if (ZSwc::isRoot(n)) {
      return colorsForDifferentTopology[0]->get();
    } else if (ZSwc::isBranchNode(n)) {
      return colorsForDifferentTopology[1]->get();
    } else if (ZSwc::isLeaf(n)) {
      return colorsForDifferentTopology[2]->get();
    } else {
      return colorsForDifferentTopology[3]->get();
    }
  } else {
    CHECK(false);
  }
}

void ZSwcColorParameters::adjustWidgets()
{
  swcTreeColor.setVisible(colorMode.isSelected("Single Color"));

  for (size_t i = 0; i < colorsForDifferentType.size(); ++i) {
    colorsForDifferentType[i]->setVisible(m_swcPack &&
                                          m_swcPack->allNodeType().find(i) != m_swcPack->allNodeType().end() &&
                                          colorMode.isSelected("Branch Type"));
  }
  for (const auto& color : colorsForSubclassType) {
    color->setVisible(colorMode.isSelected("Subclass"));
  }
  for (const auto& color : colorsForDifferentTopology) {
    color->setVisible(colorMode.isSelected("Topology"));
  }
  colorMapBranchType.setVisible(colorMode.isSelected("Colormap Branch Type"));
}

} // namespace nim
