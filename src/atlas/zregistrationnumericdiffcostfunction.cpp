#include "zregistrationnumericdiffcostfunction.h"
#include <vector>

namespace nim {

ZRegistrationNumericDiffCostFunction::ZRegistrationNumericDiffCostFunction(double relativeStepSize)
  : ZRegistrationCostFunction()
  , m_metric(nullptr)
  , m_relativeStepSize(relativeStepSize)
{
}

void ZRegistrationNumericDiffCostFunction::setMetric(ZImageToImageMetric &metric)
{
  m_metric = &metric;
  m_metric->setUseMultithreading(m_useMultithreading);
}

void ZRegistrationNumericDiffCostFunction::setUseMultithreading(bool i)
{
  ZRegistrationCostFunction::setUseMultithreading(i);
  if (m_metric)
    m_metric->setUseMultithreading(i);
}

bool ZRegistrationNumericDiffCostFunction::evaluate(const double * const parameters, double *cost, double *gradient) const
{
  if (!m_metric || !m_transform) {
    LOG(FATAL) << "Metric or Transform is not set";
  }
  // do img check in ZImgRegistration

  //LOG(INFO) << "Func Eval Once";
  //ZBenchTimer bt("Func Eval");
  //bt.start();


  IMG_TYPED_CALL_2TYPE(evaluate_Impl, (*m_fixedImg), (*m_movingImg), parameters, cost);

  if (gradient) {
    std::vector<double> paras(parameters, parameters + numParameters());
    double fallbackdelta = 0.0;
    for (size_t i=0; i<paras.size(); ++i) {
      fallbackdelta += std::abs(paras[i]) * m_relativeStepSize;
    }
    fallbackdelta = (fallbackdelta == 0) ? m_relativeStepSize : (fallbackdelta / paras.size());

    std::vector<double> paraPlusDelta = paras;
    for (size_t i=0; i<paras.size(); ++i) {
      double delta = std::abs(paras[i]) * m_relativeStepSize;
      if (delta == 0.0)
        delta = fallbackdelta;
      paraPlusDelta[i] += delta;
      double newValue = 0;
      IMG_TYPED_CALL_2TYPE(evaluate_Impl, (*m_fixedImg), (*m_movingImg), paraPlusDelta.data(), &newValue);
      gradient[i] = (newValue - *cost) / delta;
      paraPlusDelta[i] = paras[i];
    }
  }

  //bt.stopAndPrint();

  return true;
}

template<typename TFixed, typename TMoving>
void ZRegistrationNumericDiffCostFunction::evaluate_Impl(const double * const parameters, double *value) const
{
  m_transform->setParameters(parameters);
  ZImg movingBuffer(ZImgInfo(m_fixedImg->width(), m_fixedImg->height(), m_fixedImg->depth(), 1, 1, m_movingImg->bytesPerVoxel(), m_movingImg->voxelFormat()));
  if (m_transform->is2DTransform()) {
    m_transform->transformImage(m_movingImg->channelData<TMoving>(0), m_movingImg->width(), m_movingImg->height(),
                                movingBuffer.channelData<TMoving>(0), 0, m_fixedImg->width(), 0, m_fixedImg->height());
  } else {
    m_transform->transformImage(m_movingImg->channelData<TMoving>(0), m_movingImg->width(), m_movingImg->height(),
                                m_movingImg->depth(), movingBuffer.channelData<TMoving>(0), 0, m_fixedImg->width(),
                                0, m_fixedImg->height(), 0, m_fixedImg->depth());
  }
  *value = m_metric->value(m_fixedImg->channelData<TFixed>(0), movingBuffer.channelData<TMoving>(0),
                           m_fixedImg->width(), m_fixedImg->height(), m_fixedImg->depth());
}

} // namespace nim
