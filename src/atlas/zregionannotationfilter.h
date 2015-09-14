#ifndef ZREGIONANNOTATIONFILTER_H
#define ZREGIONANNOTATIONFILTER_H

#include "zobjfilter.h"
#include "zregionannotation.h"
#include "zroifilter.h"

namespace nim {

class ZRegionAnnotationFilter : public ZObjFilter
{
  Q_OBJECT
public:
  ZRegionAnnotationFilter(ZView &view);
  ~ZRegionAnnotationFilter();

  void setData(ZRegionAnnotation &regionAnnotation);

  void releaseItemsOwnership();

  void setVisible(bool v) { m_visible.set(v); }
  void setSelected(bool v) { Q_UNUSED(v); }
  void setNormalView(int z, int t) override;
  void setMaxZProjView(int t) override;

  const std::vector<int>& boundBox() const;

  ZWidgetsGroup* viewSettingWidgetsGroup();

  virtual void deleteKeyPressed() override;
  virtual void mousePressed(const QPointF &scenePos) override;
  virtual void mouseReleased(const QPointF &scenePos) override;

signals:

public slots:

protected:

private slots:
  void visibleChanged();
  void regionROIAdded(int64_t id, ZROI *roi);
  void allROIChanged();

private:
  ZRegionAnnotation *m_regionAnnotation;
  std::map<int, std::unique_ptr<ZROIFilter>> m_idToROIFilters;
  std::map<int, QString> m_idToRegionNames;
  std::map<QString, int> m_nameToID;

  ZBoolParameter m_visible;

  ZWidgetsGroup *m_widgetsGroup;
  ZView &m_view;
};

} // namespace nim

#endif // ZREGIONANNOTATIONFILTER_H
