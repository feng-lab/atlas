#include "zimagecompositetransform.h"

namespace nim {

ZImageCompositeTransform::ZImageCompositeTransform()
  : ZImageTransform()
{
}

void ZImageCompositeTransform::addTransform(const ZImageTransform &tfm)
{
  m_tfms.emplace_back(tfm.clone());
  constructParameters();
}

void ZImageCompositeTransform::addTransform(ZImageTransform *tfm)
{
  m_tfms.emplace_back(tfm);
  constructParameters();
}

size_t ZImageCompositeTransform::numParameters() const
{
  size_t res = 0;
  for (auto it = m_tfms.cbegin(); it != m_tfms.cend(); ++it) {
    res += (*it)->numParameters();
  }
  return res;
}

void ZImageCompositeTransform::setParameters(const double *para)
{
  m_parameters = std::vector<double>(para, para+numParameters());
  size_t idx = 0;
  for (auto it = m_tfms.begin(); it != m_tfms.end(); ++it) {
    (*it)->setParameters(&m_parameters[idx]);
    idx += (*it)->numParameters();
  }
}

bool ZImageCompositeTransform::is2DTransform() const
{
  for (auto it = m_tfms.cbegin(); it != m_tfms.cend(); ++it) {
    if (!(*it)->is2DTransform())
      return false;
  }
  return true;
}

void ZImageCompositeTransform::adaptParameters(size_t fromLevel, size_t toLevel)
{
  for (auto it = m_tfms.begin(); it != m_tfms.end(); ++it) {
    (*it)->adaptParameters(fromLevel, toLevel);
  }
}

std::vector<double> ZImageCompositeTransform::estimateParameterScales(const double *dims) const
{
  std::vector<double> res;
  for (auto it = m_tfms.cbegin(); it != m_tfms.cend(); ++it) {
    std::vector<double> tmpres = (*it)->estimateParameterScales(dims);
    res.insert(res.end(), tmpres.begin(), tmpres.end());
  }
  return res;
}

void ZImageCompositeTransform::transformPoint(double *inoutCoords) const
{
  for (auto it = m_tfms.crbegin(); it != m_tfms.crend(); ++it) {
    (*it)->transformPoint(inoutCoords);
  }
}

QString ZImageCompositeTransform::toQString() const
{
  QString res;
  size_t idx = 1;
  for (auto it = m_tfms.begin(); it != m_tfms.end(); ++it) {
    res += QString("Transform %1: %2\n").arg(idx++).arg((*it)->toQString());
  }
  return res;
}

ZImageTransform *ZImageCompositeTransform::clone() const
{
  ZImageCompositeTransform* res = new ZImageCompositeTransform();
  for (auto it = m_tfms.cbegin(); it != m_tfms.cend(); ++it) {
    res->addTransform(*it->get());
  }
  return res;
}

ZImageTransform *ZImageCompositeTransform::makeInverseTransform() const
{
  ZImageCompositeTransform* res = new ZImageCompositeTransform();
  for (auto it = m_tfms.crbegin(); it != m_tfms.crend(); ++it) {
    res->addTransform((*it)->makeInverseTransform());
  }
  return res;
}

void ZImageCompositeTransform::constructParameters()
{
  m_parameters.clear();
  for (auto it = m_tfms.cbegin(); it != m_tfms.cend(); ++it) {
    const std::vector<double>& tmpres = (*it)->parameters();
    m_parameters.insert(m_parameters.end(), tmpres.begin(), tmpres.end());
  }
}

} // namespace nim
