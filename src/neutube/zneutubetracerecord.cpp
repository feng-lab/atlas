#include "zneutubetracerecord.h"

#include "zlog.h"

namespace nim::neutube {

namespace {

constexpr std::uint32_t ZeroBitMask = 0u;

constexpr int TraceRecordFitScoreBit = 1;
constexpr int TraceRecordHitRegionBit = 2;
constexpr int TraceRecordIndexBit = 3;
constexpr int TraceRecordRefitBit = 4;
constexpr int TraceRecordFitHeightBit = 5;
constexpr int TraceRecordDirectionBit = 6;
constexpr int TraceRecordFixPointBit = 7;

[[nodiscard]] constexpr std::uint32_t bitMask(int bitIndex)
{
  CHECK(bitIndex >= 0);
  CHECK(bitIndex < 32);
  return 1u << static_cast<std::uint32_t>(bitIndex);
}

void setBit(int bitIndex, bool value, std::uint32_t* mask)
{
  CHECK(mask != nullptr);
  if (value) {
    *mask |= bitMask(bitIndex);
  } else {
    *mask &= ~bitMask(bitIndex);
  }
}

[[nodiscard]] bool getBit(std::uint32_t mask, int bitIndex)
{
  return (mask & bitMask(bitIndex)) != 0u;
}

} // namespace

void traceRecordReset(TraceRecord* tr)
{
  CHECK(tr != nullptr);
  tr->mask = ZeroBitMask;
  tr->fs.n = 0;
  tr->hitRegion = -1;
  tr->index = 0;
  tr->refit = 0;
  tr->direction = TraceDirection::Unknown;
  tr->fixPoint = -1.0;
  tr->fitHeight[0] = 0;
  tr->fitHeight[1] = 0;
}

void traceRecordSetScore(TraceRecord* tr, const StackFitScore& fs)
{
  if (tr == nullptr) {
    return;
  }
  setBit(TraceRecordFitScoreBit, true, &tr->mask);
  tr->fs = fs;
}

void traceRecordSetHitRegion(TraceRecord* tr, int hitRegion)
{
  if (tr == nullptr) {
    return;
  }
  setBit(TraceRecordHitRegionBit, true, &tr->mask);
  tr->hitRegion = hitRegion;
}

void traceRecordSetIndex(TraceRecord* tr, int index)
{
  if (tr == nullptr) {
    return;
  }
  setBit(TraceRecordIndexBit, true, &tr->mask);
  tr->index = index;
}

void traceRecordSetRefit(TraceRecord* tr, int refit)
{
  if (tr == nullptr) {
    return;
  }
  setBit(TraceRecordRefitBit, true, &tr->mask);
  tr->refit = refit;
}

void traceRecordSetFitHeight(TraceRecord* tr, int which, int value)
{
  if (tr == nullptr) {
    return;
  }
  CHECK(which == 0 || which == 1);
  setBit(TraceRecordFitHeightBit, true, &tr->mask);
  tr->fitHeight[static_cast<size_t>(which)] = value;
}

void traceRecordSetDirection(TraceRecord* tr, TraceDirection direction)
{
  if (tr == nullptr) {
    return;
  }
  setBit(TraceRecordDirectionBit, true, &tr->mask);
  tr->direction = direction;
}

void traceRecordSetFixPoint(TraceRecord* tr, double value)
{
  if (tr == nullptr) {
    return;
  }
  setBit(TraceRecordFixPointBit, true, &tr->mask);
  tr->fixPoint = value;
}

void traceRecordDisableFixPoint(TraceRecord* tr)
{
  if (tr == nullptr) {
    return;
  }
  setBit(TraceRecordFixPointBit, false, &tr->mask);
}

int traceRecordIndex(const TraceRecord* tr)
{
  if (tr == nullptr) {
    return -1;
  }
  if (!getBit(tr->mask, TraceRecordIndexBit)) {
    return -1;
  }
  return tr->index;
}

int traceRecordRefit(const TraceRecord* tr)
{
  if (tr == nullptr) {
    return -1;
  }
  if (!getBit(tr->mask, TraceRecordRefitBit)) {
    return 0;
  }
  return tr->refit;
}

int traceRecordFitHeight(const TraceRecord* tr, int which)
{
  if (tr == nullptr) {
    return -1;
  }
  CHECK(which == 0 || which == 1);
  if (!getBit(tr->mask, TraceRecordFitHeightBit)) {
    return 0;
  }
  return tr->fitHeight[static_cast<size_t>(which)];
}

TraceDirection traceRecordDirection(const TraceRecord* tr)
{
  if (tr == nullptr) {
    return TraceDirection::Unknown;
  }
  if (!getBit(tr->mask, TraceRecordDirectionBit)) {
    return TraceDirection::Unknown;
  }
  return tr->direction;
}

double traceRecordFixPoint(const TraceRecord* tr)
{
  if (tr == nullptr) {
    return -1.0;
  }
  if (!getBit(tr->mask, TraceRecordFixPointBit)) {
    return -1.0;
  }
  return tr->fixPoint;
}

bool traceRecordHasFixPoint(const TraceRecord* tr)
{
  if (tr == nullptr) {
    return false;
  }
  return getBit(tr->mask, TraceRecordFixPointBit);
}

} // namespace nim::neutube
