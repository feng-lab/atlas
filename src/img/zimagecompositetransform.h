#pragma once

#include "zimagetransform.h"
#include <deque>
#include <memory>

namespace nim {

class ZImageCompositeTransform : public ZImageTransform
{
public:
  void addTransform(const ZImageTransform& tfm);

  // take ownership
  void addTransform(ZImageTransform* tfm);

  // ZImageTransform interface

public:
  [[nodiscard]] size_t numParameters() const override;

  void setParameters(double const* para) override;

  [[nodiscard]] bool is2DTransform() const override;

  void adaptParameters(size_t fromLevel, size_t toLevel) override;

  std::vector<double> estimateParameterScales(const double* dims) const override;

  void transformPoint(double* inoutCoords) const override;

  [[nodiscard]] QString toQString() const override;

  [[nodiscard]] ZImageTransform* clone() const override;

  [[nodiscard]] ZImageTransform* makeInverseTransform() const override;

protected:
  void constructParameters();

protected:
  std::deque<std::unique_ptr<ZImageTransform>> m_tfms;
};

} // namespace nim
