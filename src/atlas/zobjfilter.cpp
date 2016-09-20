#include "zobjfilter.h"

#include "zlog.h"
#include "zparameter.h"

namespace nim {

ZObjFilter::ZObjFilter(ZView& view)
  : m_view(view)
  , m_offsetPara(QString("Offset"), glm::dvec4(0, 0, 0, 0),
                 glm::dvec4(std::numeric_limits<int>::min()),
                 glm::dvec4(std::numeric_limits<int>::max()))
{
  m_offsetPara.setDecimal(0);
  m_offsetPara.setSingleStep(1);
  m_offsetPara.setStyle("SPINBOX");
  connect(&m_offsetPara, &ZDVec4Parameter::valueChanged, this, &ZObjFilter::offsetChanged);
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
  boundBox[0] += m_offsetPara.get().x;
  boundBox[1] += m_offsetPara.get().x;
  boundBox[2] += m_offsetPara.get().y;
  boundBox[3] += m_offsetPara.get().y;
  boundBox[4] += m_offsetPara.get().z;
  boundBox[5] += m_offsetPara.get().z;
  boundBox[6] += m_offsetPara.get().w;
  boundBox[7] += m_offsetPara.get().w;
}

void ZObjFilter::offsetChanged()
{
  emit boundBoxChanged();
}

} // namespace nim
