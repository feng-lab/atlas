#include "zhdf5globallock.h"

namespace nim {

std::recursive_mutex& hdf5GlobalMutex()
{
  static std::recursive_mutex mutex;
  return mutex;
}

} // namespace nim
