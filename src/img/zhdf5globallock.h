#pragma once

#include <mutex>

namespace nim {

// Global lock guarding HDF5 library access (H5Cpp / H5*).
//
// We treat HDF5 as not reliably thread-safe in our build configurations.
// Callers should take this lock around any HDF5 API usage to avoid crashes
// under parallel loads (e.g. async doc loading).
std::recursive_mutex& hdf5GlobalMutex();

} // namespace nim
