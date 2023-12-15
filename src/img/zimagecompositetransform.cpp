#include "zimagecompositetransform.h"

namespace nim {

void ZImageCompositeTransform::addTransform(const ZImageTransform& tfm)
{
  if (!m_tfms.empty() && m_tfms.back()->canMergeWith(&tfm)) {
    m_tfms.back()->mergeWith(&tfm);
  } else {
    m_tfms.emplace_back(tfm.clone());
  }
}

void ZImageCompositeTransform::addTransform(ZImageTransform* tfm)
{
  if (!m_tfms.empty() && m_tfms.back()->canMergeWith(tfm)) {
    m_tfms.back()->mergeWith(tfm);
  } else {
    m_tfms.emplace_back(tfm);
  }
}

size_t ZImageCompositeTransform::numParameters() const
{
  CHECK(false);
  size_t res = 0;
  for (const auto& tfm : m_tfms) {
    res += tfm->numParameters();
  }
  return res;
}

void ZImageCompositeTransform::setParameters(const double* para)
{
  CHECK(false);
  m_parameters = std::vector<double>(para, para + numParameters());
  size_t idx = 0;
  for (const auto& tfm : m_tfms) {
    tfm->setParameters(&m_parameters[idx]);
    idx += tfm->numParameters();
  }
}

bool ZImageCompositeTransform::is2DTransform() const
{
  for (const auto& tfm : m_tfms) {
    if (!tfm->is2DTransform()) {
      return false;
    }
  }
  return true;
}

void ZImageCompositeTransform::adaptParameters(size_t fromLevel, size_t toLevel)
{
  CHECK(false);
  for (const auto& tfm : m_tfms) {
    tfm->adaptParameters(fromLevel, toLevel);
  }
}

std::vector<double> ZImageCompositeTransform::estimateParameterScales(const double* dims) const
{
  CHECK(false);
  std::vector<double> res;
  for (const auto& tfm : m_tfms) {
    std::vector<double> tmpres = tfm->estimateParameterScales(dims);
    res.insert(res.end(), tmpres.begin(), tmpres.end());
  }
  return res;
}

void ZImageCompositeTransform::transformPoint(double* inoutCoords) const
{
  for (const auto& tfm : make_reverse(m_tfms)) {
    tfm->transformPoint(inoutCoords);
  }
}

std::string ZImageCompositeTransform::toString() const
{
  if (m_tfms.size() == 1) {
    return (*m_tfms.begin())->toString();
  }
  CHECK(false);
  std::string res;
  size_t idx = 1;
  for (const auto& tfm : m_tfms) {
    res += fmt::format("Transform {}: {}\n", idx++, tfm->toString());
  }
  return res;
}

ZImageTransform* ZImageCompositeTransform::clone() const
{
  auto res = new ZImageCompositeTransform();
  for (const auto& tfm : m_tfms) {
    res->addTransform(*tfm);
  }
  return res;
}

ZImageTransform* ZImageCompositeTransform::makeInverseTransform() const
{
  auto res = new ZImageCompositeTransform();
  for (const auto& tfm : make_reverse(m_tfms)) {
    res->addTransform(tfm->makeInverseTransform());
  }
  return res;
}

} // namespace nim
