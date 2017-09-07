#pragma once

#include "zobjfilter.h"
#include "zregionannotation.h"
#include "zroifilter.h"

namespace nim {

class ZRegionAnnotationFilter : public ZObjFilter
{
Q_OBJECT
public:
  explicit ZRegionAnnotationFilter(ZView& view);

  void setData(ZRegionAnnotation& regionAnnotation);

  void releaseItemsOwnership();

  void setVisible(bool v)
  { m_visible.set(v); }

  void setSelected(bool v);

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  const ZBBox<glm::ivec4>& boundBox() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

  void deleteKeyPressed() override;

  void mousePressed(const QPointF& scenePos) override;

  void mouseReleased(const QPointF& scenePos) override;

  void rotateClockwise() override;

  void rotateCounterclockwise() override;

private:
  void visibleChanged();

  void regionROIAdded(int64_t id, ZROI* roi);

  void allROIChanged();

private:
  ZRegionAnnotation* m_regionAnnotation = nullptr;
  std::map<int, std::unique_ptr<ZROIFilter>> m_idToROIFilters;
  std::map<int, QString> m_idToRegionNames;
  std::map<QString, int> m_nameToID;

  ZBoolParameter m_visible;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  ZView& m_view;
};

} // namespace nim

