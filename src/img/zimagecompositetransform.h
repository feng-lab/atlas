#pragma once

#include "zimagetransform.h"

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

  void setParameters(const double* para) override;

  [[nodiscard]] bool is2DTransform() const override;

  void adaptParameters(size_t fromLevel, size_t toLevel) override;

  std::vector<double> estimateParameterScales(const double* dims) const override;

  void transformPoint(double* inoutCoords) const override;

  [[nodiscard]] std::string toString() const override;

  [[nodiscard]] ZImageTransform* clone() const override;

  [[nodiscard]] ZImageTransform* makeInverseTransform() const override;

protected:
  std::vector<std::unique_ptr<ZImageTransform>> m_tfms;
};

} // namespace nim
