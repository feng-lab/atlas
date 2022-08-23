#pragma once

#include "z3dfilterview.h"
#include "z3dswcfilter.h"
#include "zswcdoc.h"

namespace nim {

class Z3DSwcView : public Z3DFilterView<ZSwcDoc, Z3DSwcFilter>
{
  Q_OBJECT

public:
  Z3DSwcView(ZSwcDoc& doc, Z3DView& view);

private:
  void docSwcsAdded(const std::vector<size_t>& objs);

  void docSwcAdded(size_t id);
};

} // namespace nim
