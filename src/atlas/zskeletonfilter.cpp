#include "zskeletonfilter.h"

#include "zgraphicsscene.h"
#include "zwidgetsgroup.h"

#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QWindow>

#include <cmath>
#include <limits>

namespace nim {

ZSkeletonGraphicsItem::ZSkeletonGraphicsItem(ZSkeleton& skeleton, QGraphicsItem* parent)
  : QGraphicsItem(parent)
  , m_skeleton(skeleton)
{
  const auto& verts = m_skeleton.vertices();
  const auto& edges = m_skeleton.edges();
  m_lines.reserve(static_cast<int>(edges.size()));
  for (const glm::uvec2& e : edges) {
    if (e.x >= verts.size() || e.y >= verts.size()) {
      continue;
    }
    const glm::vec3& a = verts[e.x];
    const glm::vec3& b = verts[e.y];
    m_lines.push_back(QLineF(a.x, a.y, b.x, b.y));
  }
}

QRectF ZSkeletonGraphicsItem::boundingRect() const
{
  const qreal penWidth = 1 * m_sizeScale;
  const auto bb = m_skeleton.boundBox();
  return QRectF(bb.minCorner.x - penWidth / 2,
                bb.minCorner.y - penWidth / 2,
                bb.maxCorner.x - bb.minCorner.x + penWidth,
                bb.maxCorner.y - bb.minCorner.y + penWidth);
}

void ZSkeletonGraphicsItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget* widget)
{
  if (m_t != 0) {
    return;
  }

  if (m_selected) {
    QPen pen(QBrush(QColor(255, 255, 255)), 2);
    pen.setCosmetic(true);
    painter->setPen(pen);
    painter->drawRect(boundingRect());
  }

  const double opacityScale = opacity();

  const auto& verts = m_skeleton.vertices();
  const auto& edges = m_skeleton.edges();

  if (!m_mip) {
    QColor c = m_skeletonColor;
    c.setAlphaF(std::clamp(opacityScale, 0.0, 1.0));
    painter->setPen(QPen(c, 1 * m_sizeScale));

    for (const glm::uvec2& e : edges) {
      if (e.x >= verts.size() || e.y >= verts.size()) {
        continue;
      }
      const glm::vec3& a = verts[e.x];
      const glm::vec3& b = verts[e.y];
      if (std::abs(std::round(a.z) - m_z) > 1 && std::abs(std::round(b.z) - m_z) > 1) {
        continue;
      }
      painter->drawLine(QLineF(a.x, a.y, b.x, b.y));
    }
  }

  // drawLines to widget directly is slow; rasterize into an intermediate pixmap when possible (same as SWC).
  if (widget) {
    QColor c = m_skeletonColor;
    c.setAlphaF(std::clamp(0.5 * opacityScale, 0.0, 1.0));
    double devicePixelRatio = (widget->window() && widget->window()->windowHandle())
                                ? widget->window()->windowHandle()->devicePixelRatio()
                                : 1.0;
    QPixmap buffer(widget->width() * devicePixelRatio, widget->height() * devicePixelRatio);
    buffer.fill(QColor(0, 0, 0, 0));
    QPainter pnt(&buffer);
    pnt.setTransform(painter->combinedTransform());
    pnt.setPen(QPen(c, 0));
    pnt.drawLines(m_lines);

    painter->resetTransform();
    painter->drawPixmap(0, 0, widget->width(), widget->height(), buffer);
    painter->setTransform(pnt.combinedTransform());
  } else if (m_mip) {
    QColor c = m_skeletonColor;
    c.setAlphaF(std::clamp(0.5 * opacityScale, 0.0, 1.0));
    painter->setPen(QPen(c, 0));
    painter->drawLines(m_lines);
  }
}

ZSkeletonFilter::ZSkeletonFilter(ZView& view)
  : ZObjFilter(view)
  , m_skeletonColor("Skeleton Color", glm::vec3(1, 0, 0), glm::vec3(0), glm::vec3(1))
  , m_sizeScale("Size Scale", 1.f, .001f, std::numeric_limits<float>::max())
  , m_opacity("Opacity", .5, 0., 1.)
{
  m_skeletonColor.setStyle("COLOR");

  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZSkeletonFilter::visibleChanged);
  connect(&m_skeletonColor, &ZVec3Parameter::valueChanged, this, &ZSkeletonFilter::skeletonColorChanged);
  connect(&m_sizeScale, &ZFloatParameter::valueChanged, this, &ZSkeletonFilter::sizeScaleChanged);
  connect(&m_opacity, &ZDoubleParameter::valueChanged, this, &ZSkeletonFilter::opacityChanged);

  addParameter(&m_skeletonColor);
  m_sizeScale.setSingleStep(0.001);
  m_sizeScale.setDecimal(3);
  m_sizeScale.setStyle("SPINBOX");
  addParameter(&m_sizeScale);
  {
    const QSignalBlocker blocker(m_viewPrecedencePara);
    m_viewPrecedencePara.set(getViewPrecedence());
  }
  addParameter(&m_opacity);
}

void ZSkeletonFilter::setData(ZSkeleton& skeleton)
{
  m_skeleton = &skeleton;
  createSkeletonItem();
}

void ZSkeletonFilter::releaseItemsOwnership()
{
  static_cast<void>(m_item.release());
}

void ZSkeletonFilter::setSelected(bool v)
{
  if (m_item) {
    m_item->setSelected_(v);
  }
}

void ZSkeletonFilter::setNormalView(int z, int t)
{
  if (m_item) {
    m_item->setNormalView(realZ(z), realT(t));
  }
}

void ZSkeletonFilter::setMaxZProjView(int t)
{
  if (m_item) {
    m_item->setMaxZProjView(realT(t));
  }
}

ZBBox<glm::ivec4> ZSkeletonFilter::boundBox() const
{
  ZBBox<glm::ivec4> res;
  if (m_skeleton) {
    const auto bb = m_skeleton->boundBox();
    res.expand(glm::ivec4(static_cast<int>(std::floor(bb.minCorner.x)),
                          static_cast<int>(std::floor(bb.minCorner.y)),
                          static_cast<int>(std::floor(bb.minCorner.z)),
                          0));
    res.expand(glm::ivec4(static_cast<int>(std::ceil(bb.maxCorner.x)),
                          static_cast<int>(std::ceil(bb.maxCorner.y)),
                          static_cast<int>(std::ceil(bb.maxCorner.z)),
                          1));
    updateBoundBoxWithOffsetPara(res);
  }
  return res;
}

std::shared_ptr<ZWidgetsGroup> ZSkeletonFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("", 1);
    m_widgetsGroup->addChild(m_visible, 1);

    auto* pb = new QPushButton("Bring to Front");
    connect(pb, &QPushButton::clicked, this, &ZSkeletonFilter::bringToFront);
    m_widgetsGroup->addChild(*pb, 1);

    pb = new QPushButton("Send to Back");
    connect(pb, &QPushButton::clicked, this, &ZSkeletonFilter::sendToBack);
    m_widgetsGroup->addChild(*pb, 1);

    m_widgetsGroup->addChild(m_viewPrecedencePara, 1);
    m_widgetsGroup->addChild(m_skeletonColor, 1);
    m_widgetsGroup->addChild(m_sizeScale, 1);
    m_widgetsGroup->addChild(m_transform, 1);
    m_widgetsGroup->addChild(m_offsetPara, 1);
    m_widgetsGroup->addChild(m_opacity, 1);
  }
  return m_widgetsGroup;
}

void ZSkeletonFilter::viewPrecedenceChanged()
{
  if (m_item) {
    m_item->setZValue(m_viewPrecedencePara.get());
  }
  ZObjFilter::viewPrecedenceChanged();
}

void ZSkeletonFilter::transformChanged()
{
  if (m_item) {
    m_item->setTransform(getQTransform());
  }
  ZObjFilter::transformChanged();
}

void ZSkeletonFilter::offsetChanged()
{
  if (m_item) {
    if (m_view.isMaxZProjView()) {
      m_item->setMaxZProjView(realT());
    } else {
      m_item->setNormalView(realZ(), realT());
    }
  }
  ZObjFilter::offsetChanged();
}

void ZSkeletonFilter::createSkeletonItem()
{
  CHECK(m_skeleton);
  m_item = std::make_unique<ZSkeletonGraphicsItem>(*m_skeleton);
  m_item->setZValue(m_viewPrecedencePara.get());
  m_item->setSkeletonColor(
    QColor(m_skeletonColor.get().x * 255, m_skeletonColor.get().y * 255, m_skeletonColor.get().z * 255));
  m_item->setSizeScale(m_sizeScale.get());
  m_item->setOpacity(m_opacity.get());
  m_item->setTransform(getQTransform());
  if (m_view.isMaxZProjView()) {
    m_item->setMaxZProjView(realT());
  } else {
    m_item->setNormalView(realZ(), realT());
  }
  m_view.scene().addItem(m_item.get());
}

void ZSkeletonFilter::visibleChanged()
{
  if (m_item) {
    m_item->setVisible(m_visible.get());
  }
  Q_EMIT boundBoxChanged();
}

void ZSkeletonFilter::skeletonColorChanged()
{
  if (m_item) {
    m_item->setSkeletonColor(
      QColor(m_skeletonColor.get().x * 255, m_skeletonColor.get().y * 255, m_skeletonColor.get().z * 255));
  }
}

void ZSkeletonFilter::sizeScaleChanged()
{
  if (m_item) {
    m_item->setSizeScale(m_sizeScale.get());
  }
}

void ZSkeletonFilter::opacityChanged()
{
  if (m_item) {
    m_item->setOpacity(m_opacity.get());
  }
}

} // namespace nim
