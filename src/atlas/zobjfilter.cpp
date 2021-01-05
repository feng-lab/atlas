#include "zobjfilter.h"

#include "zlog.h"
#include "zparameter.h"

namespace nim {

ZObjFilter::ZObjFilter(ZView& view)
  : m_view(view)
  , m_visible("Visible", true)
  , m_viewPrecedencePara(QString("View Precedence"), 0,
                         std::numeric_limits<int>::min(),
                         std::numeric_limits<int>::max())
  , m_transform(QString("Transform"))
  , m_offsetPara(QString("Offset"), glm::dvec2(0),
                 glm::dvec2(std::numeric_limits<int>::min()),
                 glm::dvec2(std::numeric_limits<int>::max()))
{
  m_viewPrecedencePara.setStyle("SPINBOX");
  connect(&m_visible, &ZBoolParameter::boolChanged, this, &ZObjFilter::objVisibleChanged);
  connect(&m_viewPrecedencePara, &ZIntParameter::valueChanged, this, &ZObjFilter::viewPrecedenceChanged);
  connect(&m_transform, &Z2DTransformParameter::valueChanged, this, &ZObjFilter::transformChanged);
  std::vector<QString> names{"z", "t"};
  m_offsetPara.setNameForEachValue(names);
  m_offsetPara.setDecimal(0);
  m_offsetPara.setSingleStep(1);
  m_offsetPara.setStyle("SPINBOX");
  connect(&m_offsetPara, &ZDVec2Parameter::valueChanged, this, &ZObjFilter::offsetChanged);

  addParameter(&m_visible);
  addParameter(&m_viewPrecedencePara);
  addParameter(&m_transform);
  addParameter(&m_offsetPara);
}

void ZObjFilter::read(const json::object& json)
{
  for (auto para : m_parameters) {
    para->read(json);
  }
}

void ZObjFilter::write(json::object& json) const
{
  for (auto para : m_parameters) {
    para->write(json);
  }
}

QPointF ZObjFilter::mapFromScene(QPointF p) const
{
  bool invertible = false;
  QTransform itrans = getQTransform().inverted(&invertible);

  if (!invertible)
    LOG(WARNING) << "Can not map from scene rect, transform matrix is not invertible.";
  return itrans.map(p);
}

void ZObjFilter::addParameter(ZParameter* para)
{
  CHECK(para);
  m_parameters.push_back(para);
}

void ZObjFilter::removeParameter(ZParameter* para)
{
  erase(m_parameters, para);
}

void ZObjFilter::updateBoundBoxWithOffsetPara(ZBBox<glm::ivec4>& boundBox) const
{
  QRectF rect(boundBox.minCorner().x, boundBox.minCorner().y,
              boundBox.maxCorner().x - boundBox.minCorner().x,
              boundBox.maxCorner().y - boundBox.minCorner().y);
  QTransform trans = getQTransform();
  QRectF mappedRect = trans.mapRect(rect);
  boundBox.setMinCorner(glm::ivec4(std::floor(mappedRect.left()), std::floor(mappedRect.top()),
                                   m_offsetPara.get().x + boundBox.minCorner().z,
                                   m_offsetPara.get().y + boundBox.minCorner().w));
  boundBox.setMaxCorner(glm::ivec4(std::ceil(mappedRect.right()), std::ceil(mappedRect.bottom()),
                                   m_offsetPara.get().x + boundBox.maxCorner().z,
                                   m_offsetPara.get().y + boundBox.maxCorner().w));
}

QTransform ZObjFilter::getQTransform() const
{
  const glm::dmat3& m = m_transform.get();
  // LOG(INFO) << toQString(m) << " " << m[2][0] << " " << m[2][1];
  return QTransform(m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]);
}

QRectF ZObjFilter::mapToSceneRect(const QRectF& rect) const
{
  return getQTransform().mapRect(rect);
}

QRectF ZObjFilter::mapFromSceneRect(const QRectF& rect) const
{
  bool invertible = false;
  QTransform itrans = getQTransform().inverted(&invertible);

  if (!invertible)
    LOG(WARNING) << "Can not map from scene rect, transform matrix is not invertible.";
  return itrans.mapRect(rect);
}

void ZObjFilter::viewPrecedenceChanged()
{
}

void ZObjFilter::transformChanged()
{
  emit boundBoxChanged();
}

void ZObjFilter::offsetChanged()
{
  emit boundBoxChanged();
}

void ZObjFilter::bringToFront()
{
  m_viewPrecedencePara.set(m_view.maxViewPrecedence() + 1);
}

void ZObjFilter::sendToBack()
{
  m_viewPrecedencePara.set(m_view.minViewPrecedence() - 1);
}

} // namespace nim
