#include "z3dgeometryfilter.h"

namespace nim {

Z3DGeometryFilter::Z3DGeometryFilter(Z3DGlobalParameters& globalPara, QObject* parent)
  : Z3DBoundedFilter(globalPara, parent)
  , m_stayOnTop("Stay On Top", false)
  , m_pickingObjectsRegistered(false)
{
  addParameter(m_stayOnTop);
}

} // namespace nim
