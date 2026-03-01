#pragma once

#include "zobjfilter.h"
#include "zparameter.h"
#include "znumericparameter.h"
#include "zswcdoc.h"
#include "zgraphicsitemtype.h"
#include "zswccolorparameters.h"
#include <QGraphicsEllipseItem>
#include <QGraphicsLineItem>
#include <map>
#include <vector>

namespace nim {

class ZSwcFilter;

#if 0
class ZSwcGraphicsItem : public QGraphicsItem
{
public:
  enum
  {
    Type = GraphicsItemType::ZSwcGraphicsItem
  };

  [[nodiscard]] int type() const override
  { return Type; }

  explicit ZSwcGraphicsItem(ZSwcPack& swcPack, QGraphicsItem* parent = nullptr);

  void setShowSkeleton(bool v)
  {
    m_showSkeleton = v;
    update();
  }

  void setSkeletonColor(const QColor& c)
  {
    m_skeletonColor = c;
    update();
  }

//  void setOpacity(double v)
//  {
//    m_opacity = v;
//    update();
//  }

  void setNormalView(int z, int t)
  {
    if (m_mip || m_z != z || m_t != t) {
      m_mip = false;
      m_z = z;
      m_t = t;
      update();
    }
  }

  void setMaxZProjView(int t)
  {
    if (!m_mip || m_t != t) {
      m_mip = true;
      m_t = t;
      update();
    }
  }

  [[nodiscard]] const ZBBox<glm::ivec4>& boundBox() const
  { return m_boundBox; }

  // QGraphicsItem interface
public:
  [[nodiscard]] QRectF boundingRect() const override;

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

protected:
  ZSwcPack& m_swcPack;
  ZBBox<glm::ivec4> m_boundBox;
  bool m_showSkeleton = true;
  QColor m_skeletonColor{255, 0, 0};
  double m_opacity = 1;
  bool m_mip = false;
  int m_z = 0;
  int m_t = 0;
  QList<QLineF> m_lines;
};
#endif

class ZSwcSkeletonGraphicsItem : public QGraphicsItem
{
public:
  enum
  {
    Type = GraphicsItemType::ZSwcSkeletonGraphicsItem
  };

  [[nodiscard]] int type() const override
  {
    return Type;
  }

  explicit ZSwcSkeletonGraphicsItem(ZSwcPack& swcPack, QGraphicsItem* parent = nullptr);

  // item is not selectable (otherwise it will be hard to select swc nodes), we just fake some visual clue here
  void setSelected_(bool v)
  {
    m_selected = v;
    update();
  }

  void setShowSkeleton(bool v)
  {
    m_showSkeleton = v;
    update();
  }

  void setSkeletonColor(const QColor& c)
  {
    m_skeletonColor = c;
    update();
  }

  //  void setOpacity(double v)
  //  {
  //    m_opacity = v;
  //    update();
  //  }

  void setSizeScale(double sizeScale)
  {
    m_sizeScale = sizeScale;
    update();
  }

  void setNormalView(int z, int t)
  {
    if (m_mip || m_z != z || m_t != t) {
      m_mip = false;
      m_z = z;
      m_t = t;
      update();
    }
  }

  void setMaxZProjView(int t)
  {
    if (!m_mip || m_t != t) {
      m_mip = true;
      m_t = t;
      update();
    }
  }

  // QGraphicsItem interface

public:
  [[nodiscard]] QRectF boundingRect() const override;

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

protected:
  ZSwcPack& m_swcPack;
  bool m_showSkeleton = true;
  QColor m_skeletonColor{255, 0, 0};
  double m_opacity = 1.;
  bool m_mip = false;
  int m_z = 0;
  int m_t = 0;
  double m_sizeScale = 1.;
  QList<QLineF> m_lines;
  bool m_selected = false;
};

class ZSwcNodeGraphicsItem : public QGraphicsEllipseItem
{
public:
  enum
  {
    Type = GraphicsItemType::ZSwcNodeGraphicsItem
  };

  int type() const override
  {
    return Type;
  }

  ZSwcNodeGraphicsItem(ZSwcFilter& filter,
                       ZSwcPack& swcPack,
                       const ZSwc::SwcTreeNode& swcNode,
                       const QTransform& tfm,
                       QGraphicsItem* parent = nullptr);

  void updateValue();

  void updateRectSize();

  void setTransform_(const QTransform& tfm)
  {
    m_transform = tfm;
    updateValue();
  }

  void setSizeScale(double sizeScale)
  {
    if (m_sizeScale != sizeScale) {
      m_sizeScale = sizeScale;
      updateRectSize();
    }
  }

  void setLocked(bool l);

  [[nodiscard]] const ZSwc::SwcTreeNode& swcNode() const
  {
    return m_swcNode;
  }

  [[nodiscard]] ZSwcPack& swcPack() const
  {
    return m_swcPack;
  }

  // void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget = nullptr) override;

protected:
  void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;

private:
  ZSwcFilter& m_filter;
  ZSwcPack& m_swcPack;
  ZSwc::SwcTreeNode m_swcNode;

  QPointF m_basePos;
  QTransform m_transform;

  double m_sizeScale = 1.0;
};

class ZSwcFilter : public ZObjFilter
{
  Q_OBJECT

public:
  explicit ZSwcFilter(ZView& view);

  static int getViewPrecedence()
  {
    static int vp = 20000;
    return vp++;
  }

  void setData(ZSwcPack& swcPack);

  void releaseItemsOwnership();

  void setSelected(bool v);

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  [[nodiscard]] ZBBox<glm::ivec4> boundBox() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

  void deleteKeyPressed() override;

  void popupSwcNodeContextMenu(const ZSwc::SwcTreeNode& clickedNode, QPoint globalPos);

  [[nodiscard]] ZSwcPack* swcPack() const
  {
    return m_swcPack;
  }

  [[nodiscard]] int currentRealZ() const
  {
    return realZ();
  }

  [[nodiscard]] int currentRealT() const
  {
    return realT();
  }

protected:
  void viewPrecedenceChanged() override;

  void transformChanged() override;

  void offsetChanged() override;

  void createSwcSkeletonItem();

  void createSwcNodeItems();

  void updateItemSelectedState();

  void onSwcChanged();

  void updateSwcNodeColor();

  void onSceneItemSelectionChanged();

  void onLockedStateChanged(bool lock);

private:
  void visibleChanged();

  void showSkeletonChanged();

  void skeletonColorChanged();

  void sizeScaleChanged();

  void opacityChanged();

  void extendSwcNode();

  void connectToSwcNode();

  void moveSelectedSwcNodes();

  void moveSelectedSwcNodesToCurrentPlane();

  void estimateSwcNodeRadius();

  void addNeuronNode();

  void locateSelectedNodesIn3D();

private:
  ZSwcPack* m_swcPack = nullptr;
  std::unique_ptr<ZSwcSkeletonGraphicsItem> m_item;
  std::vector<std::unique_ptr<ZSwcNodeGraphicsItem>> m_swcNodeItems;
  std::map<ZSwc::SwcTreeNode, ZSwcNodeGraphicsItem*> m_swcNodeToItem;
  std::map<QGraphicsItem*, ZSwc::SwcTreeNode> m_itemToSwcNode;

  ZBoolParameter m_showSkeleton;
  ZVec3Parameter m_skeletonColor;
  ZSwcColorParameters m_swcColorParameters;
  ZFloatParameter m_sizeScale;
  ZDoubleParameter m_opacity;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;

  bool m_ignoreSelectionChangedSignal = false;
  bool m_skipSelectionChangedProcessing = false;

  ZSwc::SwcTreeNode m_contextMenuNode;
  QAction* m_extendSwcNodeAction = nullptr;
  QAction* m_connectToSwcNodeAction = nullptr;
  QAction* m_moveToCurrentPlaneAction = nullptr;
  QAction* m_moveSelectedNodesAction = nullptr;
  QAction* m_estimateRadiusAction = nullptr;
  QAction* m_addNeuronNodeAction = nullptr;
  QAction* m_locateNodesIn3DAction = nullptr;
};

} // namespace nim
