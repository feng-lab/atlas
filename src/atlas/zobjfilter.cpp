#include "zobjfilter.h"

#include "zparameter.h"
#include <cassert>
#include "QsLog.h"

namespace nim {

ZObjFilter::ZObjFilter(ZView &view)
  : QObject(nullptr)
  , m_view(view)
{
}

void ZObjFilter::read(const QJsonObject &json)
{
  for (int i=0; i<m_parameters.size(); ++i) {
    m_parameters[i]->read(json);
  }
}

void ZObjFilter::write(QJsonObject &json) const
{
  for (int i=0; i<m_parameters.size(); ++i) {
    m_parameters[i]->write(json);
  }
}

void ZObjFilter::addParameter(ZParameter *para)
{
  assert(para);
  m_parameters.push_back(para);
}

void ZObjFilter::removeParameter(ZParameter *para)
{
  m_parameters.removeAll(para);
}

} // namespace nim
