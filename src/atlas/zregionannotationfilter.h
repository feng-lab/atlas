#pragma once

#include "zobjfilter.h"
#include "zregionannotationpack.h"
#include "zroifilter.h"

namespace nim {

class ZRegionAnnotationFilter : public ZObjFilter
{
Q_OBJECT
public:
  explicit ZRegionAnnotationFilter(ZView& view);

  static int getViewPrecedence()
  {
    static int vp = 40000;
    return vp++;
  }

  void setData(ZRegionAnnotationPack& regionAnnotationPack);

  void releaseItemsOwnership();

  void setSelected(bool v);

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  ZBBox<glm::ivec4> boundBox() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

  void deleteKeyPressed() override;

  void copyKeyPressed() override;

  void pasteKeyPressed(int slice, QPointF point, bool hFlip, bool vFlip) override;

  void mousePressed(const QPointF& scenePos) override;

  void mouseMoved(const QPointF& scenePos) override;

  void mouseReleased(const QPointF& scenePos) override;

  void rotateClockwise() override;

  void rotateCounterclockwise() override;

private:
  void visibleChanged();

  void regionROIAdded(int64_t id, ZROI* roi);

  void allROIChanged();

  void onLockedStateChanged(bool l);

private:
  ZRegionAnnotationPack* m_regionAnnotationPack = nullptr;
  std::map<int, std::unique_ptr<ZROIFilter>> m_idToROIFilters;
  std::map<int, QString> m_idToRegionNames;
  std::map<QString, int> m_nameToID;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  ZView& m_view;

  ZBoolParameter m_highlightRegionOnMouseHover;

  std::shared_ptr<ZWidgetsGroup> m_viewSettingTreeWidgetGroup;

  size_t m_numParametersWithoutRegionSepcificParas = 0;
};

} // namespace nim

