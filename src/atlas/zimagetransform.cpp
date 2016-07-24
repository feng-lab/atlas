#include "zimagetransform.h"
#include <sstream>
#include "zlog.h"

namespace nim {

ZImageTransform::ZImageTransform()
  : m_imageInterpolation(Interpolant::Cubic, PadOption::Constant, 0.0)
  , m_useMultithreading(true)
{
}

ZImageTransform::~ZImageTransform()
{
}

void ZImageTransform::setParameters(const std::vector<double> &para)
{
  CHECK_GE(para.size(), numParameters()) << "Incorrect number of parameters.";
  setParameters(para.data());
}

std::vector<double> ZImageTransform::estimateParameterScales(const double *) const
{
  std::vector<double> optimizerScales(numParameters(), 1.0);
  return optimizerScales;
}

QString ZImageTransform::paraQString() const
{
  QString res = QString("%1").arg(m_parameters[0]);
  for (size_t i=1; i<numParameters(); ++i) {
    res += QString(" %1").arg(m_parameters[i]);
  }
  return res;
}

std::ostream& operator << (std::ostream& s, const nim::ZImageTransform& tfm)
{
  return (s << qPrintable(tfm.toQString()));
}

#ifdef _USE_QSLOG_
QDebug operator << (QDebug s, const nim::ZImageTransform& tfm)
{
  s.nospace() << tfm.toQString();
  return s.space();
}
#endif

} // namespace nim
