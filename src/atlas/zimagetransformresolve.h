#ifndef ZIMAGETRANSFORMRESOLVE_H
#define ZIMAGETRANSFORMRESOLVE_H

#include <QString>
#include <map>
#include "zimagetransform.h"
#include "zimagecompositetransform.h"

namespace nim {

// use provided absolute transform and relative transform to get final transforms for each index
// if one index has multiple relative transforms, use minimum spanning tree to find the optimal one
//
class ZImageTransformResolve
{
public:
  ZImageTransformResolve();

  // idx has absolute transform, if idx already exist, update its location
  void addFixedImage(size_t idx, const ZImageTransform *tfm);
  // idx2 has relative transform to idx1, if pair already exist, update its transform and cost
  void addImagePair(size_t fixedIdx, size_t movingIdx, const ZImageTransform *tfm, double transformCost = 0.);

  // return transform for each idx, throw ZImgException if error, caller should delete the returned transforms
  std::map<size_t, ZImageCompositeTransform*> resolve(QString *summary = nullptr) const;

private:
  std::map<size_t, const ZImageTransform*> m_idxTransforms;
  std::map<std::pair<size_t, size_t>, std::pair<const ZImageTransform*, double> > m_idxPairs;
};

} // namespace nim

#endif // ZIMAGETRANSFORMRESOLVE_H
