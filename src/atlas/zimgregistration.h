#pragma once

#include "zimg.h"
#include "zregistrationoptimizer.h"
#include "zregistrationcostfunction.h"
#include "zimagetoimagemetric.h"
#include "zimagetransform.h"

namespace nim {

class ZImgRegistration
{
public:

  const ZImg& fixedImg() const
  { return *m_fixedImg; }

  void setFixedImg(const ZImg& img)
  { m_fixedImg = &img; }

  const ZImg& movingImg() const
  { return *m_movingImg; }

  void setMovingImg(const ZImg& img)
  { m_movingImg = &img; }

  //
  void setCostFunction(ZRegistrationCostFunction& costFunction);

  //
  void setInitialTransform(ZImageTransform& tfm);

  //
  void setOptimizer(const QString& str);

  // default is true
  void setUseMultithreading(bool v)
  { m_useMultithreading = v; }

  // use multiscale registration if number of scale > 1, default is 1
  void setNumScales(int i)
  { m_numScales = std::max(1, i); }

  // return cost value
  double run();

private:
  const ZImg* m_fixedImg = nullptr;
  const ZImg* m_movingImg = nullptr;

  ZRegistrationOptimizer m_optimizer;
  ZRegistrationCostFunction* m_costFunction = nullptr;
  ZImageTransform* m_transform = nullptr;

  int m_useMultithreading = true;
  int m_numScales = 1;
};

} // namespace nim

