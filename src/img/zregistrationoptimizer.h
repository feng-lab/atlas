#pragma once

#include "zregistrationcostfunction.h"
#include <ceres/gradient_problem_solver.h>
#include <ceres/solver.h>

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

  [[nodiscard]] const std::vector<double>& currentParameters() const
  { return m_currentParameters; }

  [[nodiscard]] std::string briefReport() const
  { return m_summary.BriefReport(); }

  [[nodiscard]] std::string fullReport() const
  { return m_summary.FullReport(); }

  [[nodiscard]] double initialCost() const
  { return m_summary.initial_cost; }

  [[nodiscard]] double finalCost() const
  { return m_summary.final_cost; }

protected:
  void checkParameterNumber() const;

private:
  ZRegistrationCostFunction* m_costFunction = nullptr;
  ceres::GradientProblemSolver::Options m_options;
  ceres::GradientProblemSolver::Summary m_summary;

  std::vector<double> m_initialParameters;
  std::vector<double> m_currentParameters;
  std::vector<double> m_parameterScales;
};

} // namespace nim

