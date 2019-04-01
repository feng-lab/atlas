#include "zimagetransform.h"

#include "zlog.h"
#include <sstream>

namespace nim {

void ZImageTransform::setParameters(const std::vector<double>& para)
{
  CHECK_GE(para.size(), numParameters()) << "Incorrect number of parameters.";
  setParameters(para.data());
}

std::vector<double> ZImageTransform::estimateParameterScales(const double*) const
{
  std::vector<double> optimizerScales(numParameters(), 1.0);
  return optimizerScales;
}

QString ZImageTransform::paraQString() const
{
  QString res = QString("%1").arg(m_parameters[0]);
  for (size_t i = 1; i < numParameters(); ++i) {
    res += QString(" %1").arg(m_parameters[i]);
  }
  return res;
}

std::ostream& operator<<(std::ostream& s, const ZImageTransform& tfm)
{
  return (s << qUtf8Printable(tfm.toQString()));
}

} // namespace nim
