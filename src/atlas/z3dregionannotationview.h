#pragma once

#include "z3dfilterview.h"
#include "zregionannotationdoc.h"
#include "z3dregionannotationfilter.h"

namespace nim {

class Z3DRegionAnnotationView : public Z3DFilterView<ZRegionAnnotationDoc, Z3DRegionAnnotationFilter>
{
public:
  Z3DRegionAnnotationView(ZRegionAnnotationDoc& doc, Z3DRenderingEngine& engine);

private:
  void docRegionAnnotationsAdded(const std::vector<size_t>& objs);

  void docRegionAnnotationAdded(size_t id);
};

} // namespace nim
