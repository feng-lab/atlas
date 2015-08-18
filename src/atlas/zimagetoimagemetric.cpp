#include "zimagetoimagemetric.h"

namespace nim {

ZImageToImageMetric::ZImageToImageMetric()
  : m_type(Type::LogAbsoluteDifferences)
  , m_nbins(128)
  , m_Imin(0.0)
  , m_Imax(-2.0)
  , m_useMultithreading(true)
{
}

} // namespace nim
