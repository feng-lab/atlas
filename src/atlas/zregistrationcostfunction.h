#pragma once

#include "zimagetransform.h"
#include "zimg.h"

namespace nim {

class ZRegistrationCostFunction
{
public:
  ZRegistrationCostFunction();

  virtual ~ZRegistrationCostFunction()
  {}

  int numParameters() const
  { return m_transform ? m_transform->numParameters() : -1; }

  void setTransform(ZImageTransform& transform);

  void setImages(const ZImg& fixedImage, const ZImg& movingImage)
  {
    m_fixedImg = &fixedImage;
    m_movingImg = &movingImage;
  }

  virtual void setUseMultithreading(bool i);

  // given transform parameters, calc cost and gradient (if not nullptr), return true if success
  virtual bool evaluate(const double* const parameters, double* cost, double* gradient = nullptr) const = 0;

protected:
  ZImageTransform* m_transform;

  const ZImg* m_fixedImg;
  const ZImg* m_movingImg;

  size_t m_useMultithreading;
};

} // namespace nim

