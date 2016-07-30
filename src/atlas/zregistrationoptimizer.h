#ifndef ZREGISTRATIONOPTIMIZER_H
#define ZREGISTRATIONOPTIMIZER_H

#include "zregistrationcostfunction.h"
#include <gradient_problem_solver.h>
#include <solver.h>

namespace nim {

class ZRegistrationOptimizer
{
public:
  ZRegistrationOptimizer();

  void setCostFunction(ZRegistrationCostFunction& costFunc);

  void setLineSearchDirectionType(ceres::LineSearchDirectionType dirType);

  void setInitialParameters(const std::vector<double>& para);

  void setParameterScales(const std::vector<double>& scales);

  void minimize();

  const std::vector<double>& currentParameters() const
  { return m_currentParameters; }

  QString briefReport() const
  { return QString::fromUtf8(m_summary.BriefReport().c_str()); }

  QString fullReport() const
  { return QString::fromUtf8(m_summary.FullReport().c_str()); }

  double initialCost() const
  { return m_summary.initial_cost; }

  double finalCost() const
  { return m_summary.final_cost; }

protected:
  void checkParameterNumber() const;

private:
  ZRegistrationCostFunction* m_costFunction;
  ceres::GradientProblemSolver::Options m_options;
  ceres::GradientProblemSolver::Summary m_summary;

  std::vector<double> m_initialParameters;
  std::vector<double> m_currentParameters;
  std::vector<double> m_parameterScales;
};

} // namespace nim

#endif // ZREGISTRATIONOPTIMIZER_H
