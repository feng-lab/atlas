#include "zrandom.h"

namespace nim {

ZRandom& ZRandom::instance()
{
  thread_local ZRandom random;
  return random;
}

} // namespace nim
