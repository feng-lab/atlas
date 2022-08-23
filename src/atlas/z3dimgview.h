#pragma once

#include "z3dfilterview.h"
#include "z3dimgfilter.h"
#include "zimgdoc.h"

namespace nim {

class Z3DImgView : public Z3DFilterView<ZImgDoc, Z3DImgFilter>
{
  Q_OBJECT

public:
  Z3DImgView(ZImgDoc& doc, Z3DView& view);

private:
  void docImgsAdded(const std::vector<size_t>& objs);

  void docImgAdded(size_t id);
};

} // namespace nim
