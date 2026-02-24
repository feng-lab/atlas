#include "zneutubeswcresampler.h"

#include "zneutubeswcnodeops.h"
#include "zneutubeswcops.h"

#include "zlog.h"

#include <cmath>
#include <vector>

namespace nim::neutube {

namespace {

[[nodiscard]] double distCenters(const ZSwc::ConstSwcTreeNode& a, const ZSwc::ConstSwcTreeNode& b)
{
  const double dx = a->x - b->x;
  const double dy = a->y - b->y;
  const double dz = a->z - b->z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

[[nodiscard]] bool hasOverlap(const ZSwc::ConstSwcTreeNode& a, const ZSwc::ConstSwcTreeNode& b)
{
  const double d = distCenters(a, b);
  return d < (a->radius + b->radius);
}

[[nodiscard]] bool hasSignificantOverlap(const ZSwc::ConstSwcTreeNode& a, const ZSwc::ConstSwcTreeNode& b)
{
  const double d = distCenters(a, b);
  return (d < a->radius) || (d < b->radius);
}

[[nodiscard]] bool isWithin(const ZSwc::ConstSwcTreeNode& inner, const ZSwc::ConstSwcTreeNode& outer)
{
  const double d = distCenters(inner, outer);
  return (inner->radius + d) <= outer->radius;
}

} // namespace

ZNeutubeSwcResampler::ZNeutubeSwcResampler() = default;

int ZNeutubeSwcResampler::optimalDownsample(ZSwc* tree) const
{
  if (tree == nullptr || tree->empty()) {
    return 0;
  }

  for (auto it = tree->begin(); it != tree->end(); ++it) {
    it->weight = 1.0;
  }

  int total = 0;
  while (true) {
    const int removed = suboptimalDownsample(tree);
    if (removed <= 0) {
      break;
    }
    total += removed;
  }

  return total;
}

int ZNeutubeSwcResampler::suboptimalDownsample(ZSwc* tree) const
{
  CHECK(tree != nullptr);

  std::vector<ZSwc::SwcTreeNode> order;
  order.reserve(tree->size());
  for (auto it = tree->begin(); it != tree->end(); ++it) {
    order.push_back(it);
  }

  int removed = 0;
  for (size_t i = 0; i < order.size();) {
    const ZSwc::SwcTreeNode tn = order[i];
    const auto parent = ZSwc::parent(tn);
    if (ZSwc::isNull(parent)) {
      ++i;
      continue;
    }

    bool redundant = false;
    SwcMergeOption option = SwcMergeOption::MergeWithParent;

    if (isWithin(tn, parent)) {
      redundant = true;
    } else if (isWithin(parent, tn)) {
      redundant = true;
      option = SwcMergeOption::MergeWithChild;
    } else {
      if (isContinuation(tn) || isContinuation(parent)) {
        if (hasSignificantOverlap(tn, parent)) {
          redundant = true;
          if (isLeaf(tn)) {
            option = SwcMergeOption::MergeWithChild;
          } else {
            option = SwcMergeOption::MergeWeightedAverage;
          }
        }
      }
    }

    if (!redundant) {
      if (!m_ignoringInterRedundant) {
        if (isContinuation(tn)) {
          redundant = isInterRedundant(tn, parent);
        }
      } else {
        if (isContinuation(tn) && hasOverlap(tn, parent)) {
          redundant = isInterRedundant(tn, parent);
          if (isBranchPoint(parent) || isRoot(parent)) {
            option = SwcMergeOption::MergeWithParent;
          } else {
            option = SwcMergeOption::MergeWeightedAverage;
          }
        }
      }
    }

    if (redundant) {
      mergeToParent(tree, tn, option);
      ++removed;
      i += 2;
    } else {
      ++i;
    }
  }

  return removed;
}

bool ZNeutubeSwcResampler::isInterRedundant(const ZSwc::SwcTreeNode& tn, const ZSwc::SwcTreeNode& master) const
{
  bool redundant = false;

  const auto parent = ZSwc::parent(tn);
  const auto child = ZSwc::firstChild(tn);

  if (parent == master || child == master) {
    if (isContinuation(tn) && hasOverlap(tn, master)) {
      const double d1 = distCenters(tn, parent);
      const double d2 = distCenters(tn, child);
      const double lambda = d2 / (d1 + d2);

      SwcNode tmp = *tn;
      tmp.radius = parent->radius * lambda + child->radius * (1.0 - lambda);
      tmp.x = parent->x * lambda + child->x * (1.0 - lambda);
      tmp.y = parent->y * lambda + child->y * (1.0 - lambda);
      tmp.z = parent->z * lambda + child->z * (1.0 - lambda);

      const double dt = std::sqrt((tn->x - tmp.x) * (tn->x - tmp.x) + (tn->y - tmp.y) * (tn->y - tmp.y) +
                                  (tn->z - tmp.z) * (tn->z - tmp.z));

      if (dt * m_distanceScale < tmp.radius) {
        if ((tn->radius * m_radiusScale > tmp.radius) && (tn->radius < tmp.radius * m_radiusScale)) {
          redundant = true;
        }
      }
    }
  }

  return redundant;
}

} // namespace nim::neutube
