#pragma once

#include "zimagetoimagemetric.h"
#include "zregistrationcostfunction.h"

namespace nim {

class ZRegistrationNumericDiffCostFunction : public ZRegistrationCostFunction
{
public:
  explicit ZRegistrationNumericDiffCostFunction(double relativeStepSize = 1e-6);

  void setRelativeStepSize(double v)
  { m_relativeStepSize = v; }

  void setMetric(ZImageToImageMetric& metric);

  void setUseMultithreading(bool i) override;

  bool evaluate(const double* const parameters, double* cost, double* gradient = nullptr) const override;

private:
  template<typename TFixed, typename TMoving>
  void evaluate_Impl(const double* const parameters, double* value) const;

private:
  ZImageToImageMetric* m_metric = nullptr;
  double m_relativeStepSize;
};

} // namespace nim

