#pragma once

namespace nim {

// Port of tz_doubly_linked_list_defs.h::Dlist_Direction_e.
//
// Legacy invariants:
// - Forward + Backward == BothDir
// - Forward == -Backward
enum class TraceDirection : int
{
  Forward = 1,
  Backward = -1,
  BothDir = 0,
  Unknown = 10
};

// Port of tz_trace_defs.h tracing status codes.
enum class TraceStatus : int
{
  Normal = 0,
  HitMark = 1,
  LowScore = 2,
  TooLarge = 3,
  TooSmall = 4,
  InvalidShape = 5,
  LoopFormed = 6,
  OutOfBound = 7,
  Repeated = 8,
  OverRefit = 9,
  SeedOutOfBound = 10,
  SizeChange = 11,
  SignalChange = 12,
  Overlap = 13,
  Refit = 14,
  NotAssigned = 15
};

} // namespace nim
