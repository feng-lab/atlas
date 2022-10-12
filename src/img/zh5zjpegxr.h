#pragma once

namespace nim {

#define H5Z_FILTER_JPEGXR 488

/* ---- jpegxr_register_h5filter ----
 *
 * Register the jpegxr HDF5 filter within the HDF5 library.
 */
int jpegxr_register_h5filter();

} // namespace nim
