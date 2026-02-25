#include "zneutubelocsegchaininterpolate.h"

#include "zneutubelocsegchainknot.h"
#include "zneutubeneuroseg.h"
#include "zneutubetracerecord.h"
#include "zneutubeswcgeom.h"

#include "zlog.h"

#include <cmath>
#include <limits>
#include <optional>

namespace nim::neutube {

namespace {

[[nodiscard]] double geo3dDistLegacyLike(const std::array<double, 3>& a, const std::array<double, 3>& b)
{
  return geo3dDist(a[0], a[1], a[2], b[0], b[1], b[2]);
}

[[nodiscard]] double neurosegRyTLegacyLike(const Neuroseg& seg, double t)
{
  // Port of tz_neuroseg.c::Neuroseg_Ry_T().
  return neurosegRadiusLegacyLike(seg, t * (seg.h - 1.0));
}

} // namespace

int locsegChainInterpolateLLegacyLike(LocsegChain& chain,
                                      const std::array<double, 3>& pt,
                                      const std::array<double, 3>* ort,
                                      std::array<double, 3>* newPos)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_Interpolate_L().
  const auto kaOpt = locsegChainToKnotArrayLegacyLike(chain);
  CHECK(kaOpt.has_value()) << "locsegChainInterpolateLLegacyLike requires a non-empty chain";

  const LocsegChainKnotArrayLegacyLike& ka = *kaOpt;
  const int n = static_cast<int>(ka.knots.size());

  if (n == 1) {
    return 0;
  }

  double minDist = std::numeric_limits<double>::infinity();
  int minIndex = 0;
  double lambda = 0.0;

  std::array<double, 3> startPos{};
  std::array<double, 3> endPos{};
  double lambda1 = 0.0;
  double lambda2 = 0.0;

  if (ort == nullptr) {
    minIndex = 0;
    startPos = locsegChainKnotPosLegacyLike(ka, 0);
    endPos = locsegChainKnotPosLegacyLike(ka, 1);

    minDist = geo3dPointLineSegDist(pt, startPos, endPos, lambda1);
    lambda = lambda1;

    for (int i = 2; i < n; ++i) {
      startPos = endPos;
      endPos = locsegChainKnotPosLegacyLike(ka, i);
      const double dist = geo3dPointLineSegDist(pt, startPos, endPos, lambda1);
      if (dist < minDist) {
        minDist = dist;
        minIndex = i - 1;
        lambda = lambda1;
      }
    }
  } else {
    std::array<double, 3> segStart{};
    std::array<double, 3> segEnd{};
    for (int i = 0; i < 3; ++i) {
      segStart[static_cast<size_t>(i)] = pt[static_cast<size_t>(i)] - (*ort)[static_cast<size_t>(i)] * 5.0;
      segEnd[static_cast<size_t>(i)] = pt[static_cast<size_t>(i)] + (*ort)[static_cast<size_t>(i)] * 5.0;
    }

    minIndex = 0;
    startPos = locsegChainKnotPosLegacyLike(ka, 0);
    endPos = locsegChainKnotPosLegacyLike(ka, 1);

    int cond = 0;
    minDist = geo3dLineSegLineSegDistLegacyLike(startPos, endPos, segStart, segEnd, lambda1, lambda2, cond);
    lambda = lambda1;

    for (int i = 2; i < n; ++i) {
      startPos = endPos;
      endPos = locsegChainKnotPosLegacyLike(ka, i);
      const double dist = geo3dLineSegLineSegDistLegacyLike(startPos, endPos, segStart, segEnd, lambda1, lambda2, cond);
      if (dist < minDist) {
        minDist = dist;
        minIndex = i - 1;
        lambda = lambda1;
      }
    }
  }

  if (lambda > 0.0 && lambda < 1.0) {
    startPos = locsegChainKnotPosLegacyLike(ka, minIndex);
    endPos = locsegChainKnotPosLegacyLike(ka, minIndex + 1);
    const double len = geo3dDistLegacyLike(startPos, endPos);

    if ((len * lambda < 1.0) && (lambda <= 0.5)) {
      lambda = 0.0;
    } else if ((len * (1.0 - lambda) < 1.0) && (lambda >= 0.5)) {
      lambda = 1.0;
    } else {
      if (minIndex == 0) {
        if (len * lambda < 3.0) {
          lambda = 0.0;
        }
      } else if (minIndex == n - 2) {
        if (len * (1.0 - lambda) < 3.0) {
          lambda = 1.0;
        }
      }
    }
  }

  int index = -1;

  if (lambda > 0.0 && lambda < 1.0) {
    const std::array<double, 3> insertion = geo3dLineSegBreak(startPos, endPos, lambda);

    const auto* knot = locsegChainKnotArrayAtLegacyLike(ka, minIndex);
    const auto* nextKnot = locsegChainKnotArrayAtLegacyLike(ka, minIndex + 1);
    CHECK(knot != nullptr);
    CHECK(nextKnot != nullptr);

    std::optional<LocalNeuroseg> insertedSeg;
    TraceRecord insertedTr{};
    traceRecordReset(insertedTr);

    if (nextKnot->id == knot->id) {
      // break in the same locseg
      CHECK((knot->offset == 0.0) || (nextKnot->offset == 1.0)) << "Invalid knots";

      LocalNeuroseg* prevLocseg = chain.segAt(knot->id);
      CHECK(prevLocseg != nullptr);

      if (prevLocseg->seg.h > 1.0) {
        const double alpha = knot->offset * (1.0 - lambda) + nextKnot->offset * lambda;

        insertedSeg = *prevLocseg;
        localNeurosegChopLegacyLike(*prevLocseg, alpha);
        localNeurosegChopLegacyLike(*insertedSeg, -alpha);

        LocsegNode* node = chain.nodeAt(knot->id);
        CHECK(node != nullptr);

        if (knot->offset == 0.0) {
          traceRecordSetFixPoint(node->tr, 1.0);
          traceRecordSetFixPoint(insertedTr, nextKnot->offset * (1.0 - alpha) / (1.0 - alpha * nextKnot->offset));
        } else if (nextKnot->offset == 1.0) {
          traceRecordSetFixPoint(node->tr, knot->offset / (knot->offset + alpha * (1.0 - knot->offset)));
          traceRecordSetFixPoint(insertedTr, 0.0);
        } else {
          CHECK(false) << "Invalid knots for same-id interpolation";
        }

        LocsegNode newNode;
        newNode.locseg = *insertedSeg;
        newNode.tr = insertedTr;
        (void)chain.insertNodeAt(knot->id + 1, std::move(newNode));
        index = knot->id + 1;
      }
    } else {
      // break in different segments
      LocsegNode* node1 = chain.nodeAt(knot->id);
      LocsegNode* node2 = chain.nodeAt(nextKnot->id);
      CHECK(node1 != nullptr);
      CHECK(node2 != nullptr);

      const double r1 = neurosegRyTLegacyLike(node1->locseg.seg, knot->offset);
      const double r2 = neurosegRyTLegacyLike(node2->locseg.seg, nextKnot->offset);

      LocalNeuroseg locseg = node1->locseg;
      setNeurosegPositionLegacyLike(locseg, insertion, NeuroposReferenceLegacyLike::Bottom);
      locseg.seg.h = 1.0;
      if (r1 != r2) {
        locseg.seg.r1 = r1 * (1.0 - lambda) + r2 * lambda;
      }

      traceRecordSetFixPoint(node2->tr, nextKnot->offset);

      traceRecordSetFixPoint(insertedTr, 0.0);

      LocsegNode newNode;
      newNode.locseg = locseg;
      newNode.tr = insertedTr;
      (void)chain.insertNodeAt(nextKnot->id, std::move(newNode));
      index = nextKnot->id;

      insertedSeg = locseg;
    }

    if (newPos != nullptr) {
      CHECK(insertedSeg.has_value()) << "Expected inserted segment when lambda is in (0, 1)";
      *newPos = localNeurosegBottomLegacyLike(*insertedSeg);
    }
  } else {
    // break on knot, update newPos
    if (newPos != nullptr) {
      if (lambda == 0.0) {
        *newPos = locsegChainKnotPosLegacyLike(ka, minIndex);
      } else {
        *newPos = locsegChainKnotPosLegacyLike(ka, minIndex + 1);
      }
    }
  }

  return index;
}

} // namespace nim::neutube
