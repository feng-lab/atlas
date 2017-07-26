#include "zobjfilter.h"

#include "zlog.h"
#include "zparameter.h"

namespace nim {

ZObjFilter::ZObjFilter(ZView& view)
  : m_view(view)
  , m_viewPrecedencePara(QString("View Precedence"), 0,
                         std::numeric_limits<int>::min(),
                         std::numeric_limits<int>::max())
  , m_transform(QString("Transform"))
  , m_offsetPara(QString("Offset"), glm::dvec2(0),
                 glm::dvec2(std::numeric_limits<int>::min()),
                 glm::dvec2(std::numeric_limits<int>::max()))
{
  m_viewPrecedencePara.setStyle("SPINBOX");
  connect(&m_viewPrecedencePara, &ZIntParameter::valueChanged, this, &ZObjFilter::viewPrecedenceChanged);
  connect(&m_transform, &Z2DTransformParameter::valueChanged, this, &ZObjFilter::transformChanged);
  QList<QString> names;
  names << "z" << "t";
  m_offsetPara.setNameForEachValue(names);
  m_offsetPara.setDecimal(0);
  m_offsetPara.setSingleStep(1);
  m_offsetPara.setStyle("SPINBOX");
  connect(&m_offsetPara, &ZDVec2Parameter::valueChanged, this, &ZObjFilter::offsetChanged);
}

void ZObjFilter::read(const QJsonObject& json)
{
  for (auto para : m_parameters) {
    para->read(json);
  }
}

void ZObjFilter::write(QJsonObject& json) const
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
  m_parameters.removeAll(para);
}

void ZObjFilter::updateBoundBoxWithOffsetPara(std::array<int, 8>& boundBox) const
{
  QRectF rect(boundBox[0], boundBox[2], boundBox[1] - boundBox[0], boundBox[3] - boundBox[2]);
  QTransform trans = getQTransform();
  QRectF mappedRect = trans.mapRect(rect);
  boundBox[0] = std::floor(mappedRect.left());
  boundBox[1] = std::ceil(mappedRect.right());
  boundBox[2] = std::floor(mappedRect.top());
  boundBox[3] = std::ceil(mappedRect.bottom());

  boundBox[4] += m_offsetPara.get().x;
  boundBox[5] += m_offsetPara.get().x;
  boundBox[6] += m_offsetPara.get().y;
  boundBox[7] += m_offsetPara.get().y;
}

QTransform ZObjFilter::getQTransform() const
{
  const glm::dmat3& m = m_transform.get();
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
