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

std::ostream& operator<<(std::ostream& s, const ZImageTransform& tfm)
{
  return (s << qUtf8Printable(tfm.toQString()));
}

} // namespace nim
