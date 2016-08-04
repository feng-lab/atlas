#include "zregistrationoptimizer.h"

#include <ceres/autodiff_cost_function.h>
#include <ceres/numeric_diff_cost_function.h>
#include <ceres/gradient_problem.h>
#include <ceres/problem.h>
#include <ceres/local_parameterization.h>

namespace {

using namespace nim;

struct ScalarCostFunctor
{
  ScalarCostFunctor(const ZRegistrationCostFunction& costFunction,
                    const std::vector<double>& scales)
    : m_costFunc(costFunction)
    , m_scales(scales)
  {}

  bool operator()(const double* const x, double* residuals) const
  {
    std::vector<double> parameters(m_costFunc.numParameters());
    for (size_t i = 0; i < parameters.size(); ++i)
      parameters[i] = x[i] * m_scales[i];
    m_costFunc.evaluate(parameters.data(), residuals);
    return true;
  }

private:
  const ZRegistrationCostFunction& m_costFunc;
  std::vector<double> m_scales;
};

class CostFunctionAdaptor : public ceres::CostFunction
{
public:
  CostFunctionAdaptor(const ZRegistrationCostFunction& costFunction,
                      const std::vector<double>& scales,
                      double relativeStepSize = 1e-6)
    : CostFunction()
    , m_costFunc(costFunction)
    , m_scales(scales)
    , m_relativeStepSize(relativeStepSize)
  {
    std::vector<int32_t>* parameter_block_sizes = mutable_parameter_block_sizes();
    parameter_block_sizes->resize(1);
    parameter_block_sizes->at(0) = m_costFunc.numParameters();
    set_num_residuals(1);
  }

  virtual bool Evaluate(double const* const* para,
                        double* residuals,
                        double** jacobians) const override
  {
    std::vector<double> parameters(m_costFunc.numParameters());
    double fallbackdelta = 0.0;
    for (size_t i = 0; i < parameters.size(); ++i) {
      parameters[i] = para[0][i] * m_scales[i];
      fallbackdelta += std::abs(para[0][i]) * m_relativeStepSize;
    }

    m_costFunc.evaluate(parameters.data(), residuals);

    if (jacobians) {
      fallbackdelta = (fallbackdelta == 0) ? m_relativeStepSize : (fallbackdelta / parameters.size());
      std::vector<double> paraPlusDelta = parameters;
      for (size_t i = 0; i < parameters.size(); ++i) {
        double delta = std::abs(para[0][i]) * m_relativeStepSize;
        if (delta == 0.0)
          delta = fallbackdelta;
        paraPlusDelta[i] += delta * m_scales[i];
        double newValue;
        m_costFunc.evaluate(paraPlusDelta.data(), &newValue);
        jacobians[0][i] = (newValue - residuals[0]) / delta;
        paraPlusDelta[i] = parameters[i];
      }
    }
    return true;
  }

private:
  const ZRegistrationCostFunction& m_costFunc;
  const std::vector<double>& m_scales;
  double m_relativeStepSize;
};

class FirstOrderFunctionAdaptor : public ceres::FirstOrderFunction
{
public:
  FirstOrderFunctionAdaptor(const ZRegistrationCostFunction& costFun)
    : FirstOrderFunction()
    , m_costFun(costFun)
  {}

  virtual bool Evaluate(const double* const parameters,
                        double* cost,
                        double* gradient) const override
  {
    return m_costFun.evaluate(parameters, cost, gradient);
  }

  virtual int NumParameters() const override
  { return m_costFun.numParameters(); }

private:
  const ZRegistrationCostFunction& m_costFun;
};

}

namespace nim {

ZRegistrationOptimizer::ZRegistrationOptimizer()
  : m_costFunction(nullptr)
{
  m_options.line_search_direction_type = ceres::LBFGS;
  m_options.line_search_type = ceres::WOLFE;
  m_options.function_tolerance = 1e-8;
  m_options.max_lbfgs_rank = 100;
  m_options.max_num_iterations = 100;
}

void ZRegistrationOptimizer::setCostFunction(ZRegistrationCostFunction& costFunc)
{
  m_costFunction = &costFunc;
}

void ZRegistrationOptimizer::setLineSearchDirectionType(ceres::LineSearchDirectionType dirType)
{
  m_options.line_search_direction_type = dirType;
}

void ZRegistrationOptimizer::setInitialParameters(const std::vector<double>& para)
{
  m_initialParameters = para;
}

void ZRegistrationOptimizer::setParameterScales(const std::vector<double>& scales)
{
  m_parameterScales = scales;
}

void ZRegistrationOptimizer::minimize()
{
  CHECK_NOTNULL(m_costFunction);
  checkParameterNumber();

  m_currentParameters = m_initialParameters;
  ceres::GradientProblem problem(new FirstOrderFunctionAdaptor(*m_costFunction));
  ceres::Solve(m_options, problem, m_currentParameters.data(), &m_summary);
}

void ZRegistrationOptimizer::checkParameterNumber() const
{
  if (m_costFunction && static_cast<int>(m_initialParameters.size()) != m_costFunction->numParameters()) {
    LOG(FATAL) << "Number of Optimizer Parameters don't match number of Cost Function Parameters.";
  }
  if (!m_parameterScales.empty() && m_parameterScales.size() != m_initialParameters.size()) {
    LOG(FATAL) << "Number of Parameter Scales don't match number of Parameters.";
  }
}

} // namespace nim
