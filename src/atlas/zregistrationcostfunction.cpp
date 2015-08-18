#include "zregistrationcostfunction.h"

namespace nim {

ZRegistrationCostFunction::ZRegistrationCostFunction()
  : m_transform(nullptr)
  , m_fixedImg(nullptr)
  , m_movingImg(nullptr)
{
}

void ZRegistrationCostFunction::setUseMultithreading(bool i)
{
  m_useMultithreading = i;
  if (m_transform)
    m_transform->setUseMultithreading(i);
}

void ZRegistrationCostFunction::setTransform(ZImageTransform &transform)
{
  m_transform = &transform;
  m_transform->setUseMultithreading(m_useMultithreading);
}

} // namespace nim
