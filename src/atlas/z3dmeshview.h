#pragma once

#include "z3dfilterview.h"
#include "z3dmeshfilter.h"
#include "zmeshdoc.h"

namespace nim {

class Z3DMeshView : public Z3DFilterView<ZMeshDoc, Z3DMeshFilter>
{
public:
  Z3DMeshView(ZMeshDoc& doc, Z3DRenderingEngine& engine);

private:
  void docMeshesAdded(const std::vector<size_t>& objs);

  void docMeshAdded(size_t id);
};

} // namespace nim
