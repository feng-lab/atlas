#include "zrandom.h"

#include <tbb/enumerable_thread_specific.h>

#if defined(__aarch64__) || defined(_M_ARM64)

#include <time.h>
static uint64_t rdtsc()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  // Convert the timespec to a single 64-bit value.
  // Note that this will overflow in about 584 years since Unix Epoch.
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

#elif defined(_WIN32) || defined(_WIN64)

#include <intrin.h>
#define rdtsc __rdtsc

#else

//  For everything else
static unsigned long long rdtsc()
{
  uint32_t lo, hi;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

#endif

namespace nim {

ZRandom::ZRandom()
{
  m_eng.seed(rdtsc());
  // LOG(ERROR) << "a new zrandom!";
}

ZRandom& ZRandom::instance()
{
  static tbb::enumerable_thread_specific<ZRandom> globalZRandom;
  return globalZRandom.local();
}

} // namespace nim
