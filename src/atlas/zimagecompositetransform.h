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
  virtual size_t numParameters() const override;

  virtual void setParameters(double const* para) override;

  virtual bool is2DTransform() const override;

  virtual void adaptParameters(size_t fromLevel, size_t toLevel) override;

  virtual std::vector<double> estimateParameterScales(const double* dims) const override;

  virtual void transformPoint(double* inoutCoords) const override;

  virtual QString toQString() const override;

  virtual ZImageTransform* clone() const override;

  virtual ZImageTransform* makeInverseTransform() const override;

protected:
  void constructParameters();

protected:
  std::deque<std::unique_ptr<ZImageTransform>> m_tfms;
};

} // namespace nim

