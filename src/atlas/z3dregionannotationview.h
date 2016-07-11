#ifndef Z3DREGIONANNOTATIONVIEW_H
#define Z3DREGIONANNOTATIONVIEW_H

#include "z3dfilterview.h"
#include "zregionannotationdoc.h"
#include "z3dregionannotationfilter.h"

namespace nim {

class Z3DRegionAnnotationView : public Z3DFilterView<ZRegionAnnotationDoc, Z3DRegionAnnotationFilter>
{
  Q_OBJECT
public:
  Z3DRegionAnnotationView(ZRegionAnnotationDoc &doc, Z3DView &view);

private slots:
  void docRegionAnnotationsAdded(const QList<size_t> &objs);
  void docRegionAnnotationAdded(size_t id);

private:
};

} // namespace nim


#endif // Z3DREGIONANNOTATIONDOC_H
