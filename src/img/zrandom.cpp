#include "zrandom.h"

#include <folly/chrono/Hardware.h>
#include <tbb/enumerable_thread_specific.h>

namespace nim {

ZRandom::ZRandom()
{
  m_eng.seed(folly::hardware_timestamp());
  // LOG(ERROR) << "a new zrandom!";
}

ZRandom& ZRandom::instance()
{
  static tbb::enumerable_thread_specific<ZRandom> globalZRandom;
  return globalZRandom.local();
}

} // namespace nim
