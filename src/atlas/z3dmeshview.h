#ifndef Z3DMESHVIEW_H
#define Z3DMESHVIEW_H

#include "z3dfilterview.h"
#include "zmeshdoc.h"
#include "z3dmeshfilter.h"

namespace nim {

class Z3DMeshView : public Z3DFilterView<ZMeshDoc, Z3DMeshFilter>
{
  Q_OBJECT
public:
  Z3DMeshView(ZMeshDoc &doc, Z3DView &view);

private:
  void docMeshesAdded(const QList<size_t> &objs);
  void docMeshAdded(size_t id);
};

} // namespace nim

#endif // Z3DMESHVIEW_H
