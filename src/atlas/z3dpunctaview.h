#ifndef Z3DPUNCTAVIEW_H
#define Z3DPUNCTAVIEW_H

#include "z3dfilterview.h"
#include "zpunctadoc.h"
#include "z3dpunctafilter.h"

namespace nim {

class Z3DPunctaView : public Z3DFilterView<ZPunctaDoc, Z3DPunctaFilter>
{
  Q_OBJECT
public:
  Z3DPunctaView(ZPunctaDoc &doc, Z3DView &view);

private slots:
  void docPunctaAdded(const QList<size_t> &objs);
  void docPunctaAdded(size_t id);

private:
};

} // namespace nim

#endif // Z3DPUNCTAVIEW_H
