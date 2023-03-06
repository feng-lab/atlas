#pragma once

#include "z3dfilterview.h"
#include "z3dpunctafilter.h"
#include "zpunctadoc.h"

namespace nim {

class Z3DPunctaView : public Z3DFilterView<ZPunctaDoc, Z3DPunctaFilter>
{
public:
  Z3DPunctaView(ZPunctaDoc& doc, Z3DRenderingEngine& engine);

private:
  void docPunctasAdded(const std::vector<size_t>& objs);

  void docPunctaAdded(size_t id);
};

} // namespace nim
