#pragma once

#include "zneutubetracedefs.h"

#include <array>
#include <cstdint>

namespace nim::neutube {

// Port of tz_geo3d_scalar_field.h::Stack_Fit_Score.
struct StackFitScore
{
  int n = 0;
  std::array<double, 10> scores{};
  std::array<int, 10> options{};
};

// Port of tz_trace_defs.h::Trace_Record with its bitmask semantics.
struct TraceRecord
{
  std::uint32_t mask = 0;
  StackFitScore fs{};
  int hitRegion = -1;
  int index = 0;
  int refit = 0;
  std::array<int, 2> fitHeight = {0, 0};
  TraceDirection direction = TraceDirection::Unknown;
  double fixPoint = -1.0;
};

// Port of legacy setter/getter helpers from tz_trace_utils.c.
void traceRecordReset(TraceRecord& tr);
void traceRecordSetScore(TraceRecord& tr, const StackFitScore& fs);
void traceRecordSetHitRegion(TraceRecord& tr, int hitRegion);
void traceRecordSetIndex(TraceRecord& tr, int index);
void traceRecordSetRefit(TraceRecord& tr, int refit);
void traceRecordSetFitHeight(TraceRecord& tr, int which, int value);
void traceRecordSetDirection(TraceRecord& tr, TraceDirection direction);
void traceRecordSetFixPoint(TraceRecord& tr, double value);
void traceRecordDisableFixPoint(TraceRecord& tr);

[[nodiscard]] int traceRecordIndex(const TraceRecord& tr);
[[nodiscard]] int traceRecordRefit(const TraceRecord& tr);
[[nodiscard]] int traceRecordFitHeight(const TraceRecord& tr, int which);
[[nodiscard]] TraceDirection traceRecordDirection(const TraceRecord& tr);
[[nodiscard]] double traceRecordFixPoint(const TraceRecord& tr);
[[nodiscard]] bool traceRecordHasFixPoint(const TraceRecord& tr);

} // namespace nim::neutube
