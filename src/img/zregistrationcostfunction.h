#pragma once

#include "zimagetransform.h"
#include "zimg.h"

namespace nim {

class ZRegistrationCostFunction
{
public:
  virtual ~ZRegistrationCostFunction() = default;

  [[nodiscard]] size_t numParameters() const
  {
    return m_transform ? m_transform->numParameters() : 0;
  }

  void setTransform(ZImageTransform& transform);

  void setImages(const ZImg& fixedImage, const ZImg& movingImage)
  {
    m_fixedImg = &fixedImage;
    m_movingImg = &movingImage;
  }

  virtual void setUseMultithreading(bool i);

  // given transform parameters, calc cost and gradient (if not nullptr), return true if success
  virtual bool evaluate(const double* parameters, double* cost, double* gradient = nullptr) const = 0;

protected:
  ZImageTransform* m_transform = nullptr;

  const ZImg* m_fixedImg = nullptr;
  const ZImg* m_movingImg = nullptr;

  bool m_useMultithreading = true;
};

} // namespace nim
