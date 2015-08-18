#ifndef ZIMGFILTER_H
#define ZIMGFILTER_H

#include "zobjfilter.h"
#include <QList>
#include <vector>
#include "zparameter.h"
#include "znumericparameter.h"

class QGraphicsPixmapItem;
class ZWidgetsGroup;

namespace nim {

class ZImg;
class ZImgPack;
class ZImgPackDisplay;
class ZDoubleSpanParameter;
class ZVec3Parameter;
class ZDVec4Parameter;
class ZDoubleParameter;

class ZImgFilter : public ZObjFilter
{
  Q_OBJECT
public:
  ZImgFilter(ZView &view);
  ~ZImgFilter();

  void setData(ZImgPack &pack);

  void releaseItemsOwnership() { m_imgItems.clear(); }

  void setVisible(bool v) { m_visible.set(v); }
  void setSelected(bool v) { Q_UNUSED(v); }
  void setNormalView(int z, int t);
  void setMaxZProjView(int t);
  void setViewport(const QRectF &rect, double scale) override;

  inline bool isVisible() const { return m_isVisible; }

  std::vector<int> boundBox() const;

  // location within img, can be out of img range
  int imgSlice() const;
  int imgTime() const;

  ZWidgetsGroup* viewSettingWidgetsGroup();

signals:

public slots:

protected:
  void updateViewSettingWidgetsGroup();

private slots:
  void channelVisibleChanged();
  void channelRangeChanged();
  void channelColorChanged();
  void offsetChanged();
  void opacityChanged();
  void visibleChanged();

private:
  ZImgPackDisplay* getDisplay() const;
  void hideImgItems();
  void destroyImgItems();
  void updateImgItems();

  double getLowerChannelRange(size_t c) const;
  double getUpperChannelRange(size_t c) const;

private:
  ZImgPack* m_imgPack;

  QList<QGraphicsPixmapItem*> m_imgItems;

  ZBoolParameter m_visible;
  bool m_sliceValid;
  bool m_hasVisibleChannel;
  bool m_isVisible;

  QList<ZBoolParameter*> m_channelVisibleParas;
  QList<ZDoubleSpanParameter*> m_doubleChannelRangeParas;
  QList<ZVec3Parameter*> m_channelColorParas;
  ZDVec4Parameter *m_offsetPara;
  ZDoubleParameter m_opacity;

  ZImgPackDisplay *m_display;
  mutable ZImgPackDisplay *m_maxZProjDisplay;
  bool m_displayValid;

  ZImgPackDisplay *m_lastDisplay;
  int m_lastSlice;
  int m_lastTime;
  double m_lastScale;
  QRectF m_lastViewport;

  ZWidgetsGroup *m_widgetsGroup;
};

} // namespace nim

#endif // ZIMGFILTER_H
