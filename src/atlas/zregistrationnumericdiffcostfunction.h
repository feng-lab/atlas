#ifndef ZREGISTRATIONNUMERICDIFFCOSTFUNCTION_H
#define ZREGISTRATIONNUMERICDIFFCOSTFUNCTION_H

#include "zregistrationcostfunction.h"
#include "zimagetoimagemetric.h"

namespace nim {

class ZRegistrationNumericDiffCostFunction : public ZRegistrationCostFunction
{
public:
  ZRegistrationNumericDiffCostFunction(double relativeStepSize = 1e-6);

  void setRelativeStepSize(double v) { m_relativeStepSize = v; }
  void setMetric(ZImageToImageMetric &metric);

  virtual void setUseMultithreading(bool i) override;
  virtual bool evaluate(const double * const parameters, double *cost, double *gradient = nullptr) const override;

private:
  template<typename TFixed, typename TMoving>
  void evaluate_Impl(const double * const parameters, double *value) const;

private:
  ZImageToImageMetric *m_metric;
  double m_relativeStepSize;
};

} // namespace nim

#endif // ZREGISTRATIONNUMERICDIFFCOSTFUNCTION_H
