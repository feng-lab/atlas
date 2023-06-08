if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64.*|AARCH64.*|arm64.*|ARM64.*)")
  set(AARCH64 1)
endif ()

if (AARCH64)

set(BLAS_FOUND OFF CACHE BOOL "")
find_package(TBB REQUIRED tbb)
# print_target_properties(TBB::tbb)

else (AARCH64)

if (NOT INTEL_PATH)
  if (WIN32)
    set(INTEL_PATH "C:\\Program Files (x86)\\Intel\\oneAPI")
  else (WIN32)
    set(INTEL_PATH /opt/intel/oneapi)
  endif (WIN32)
endif ()
message(STATUS "INTEL_PATH: ${INTEL_PATH}")

if (NOT APPLE)
  set(TBB_DIR ${INTEL_PATH}/tbb/latest/lib/cmake/tbb)
endif ()
find_package(TBB REQUIRED tbb)
# print_target_properties(TBB::tbb)

if (WIN32)
  set(MKL_PATH "${INTEL_PATH}\\mkl\\latest")
else (WIN32)
  set(MKL_PATH ${INTEL_PATH}/mkl/latest)
endif (WIN32)
set(MKL_INCLUDE_DIRS ${MKL_INCLUDE_DIRS} ${MKL_PATH}/include ${MKL_PATH}/include/fftw)
if (WIN32)
  # todo: fix, mkl_tbb_thread links to static version of msvc runtime so we can not use it now
  set(MKL_LIBRARIES ${MKL_LIBRARIES}
      ${MKL_PATH}/lib/intel64/mkl_intel_lp64.lib
      ${MKL_PATH}/lib/intel64/mkl_sequential.lib
      ${MKL_PATH}/lib/intel64/mkl_core.lib)
elseif (APPLE)
  set(MKL_LIBRARIES ${MKL_LIBRARIES}
      ${MKL_PATH}/lib/libmkl_intel_lp64.a
      ${MKL_PATH}/lib/libmkl_tbb_thread.a
      ${MKL_PATH}/lib/libmkl_core.a)
else ()
  set(MKL_LIBRARIES ${MKL_LIBRARIES}
      ${MKL_PATH}/lib/intel64/libmkl_intel_lp64.a
      ${MKL_PATH}/lib/intel64/libmkl_tbb_thread.a
      ${MKL_PATH}/lib/intel64/libmkl_core.a)
endif ()

message(STATUS "MKL_INCLUDE_DIRS: ${MKL_INCLUDE_DIRS}")
message(STATUS "MKL_LIBRARIES: ${MKL_LIBRARIES}")

set(BLAS_FOUND ON CACHE BOOL "")
if (APPLE)
  set(BLAS_LIBRARIES ${MKL_LIBRARIES}  TBB::tbb -lc++)
else ()
  set(BLAS_LIBRARIES ${MKL_LIBRARIES}  TBB::tbb)
endif ()
set(BLAS_INCLUDE_DIRS ${MKL_INCLUDE_DIRS})
set(BLAS_LINKER_FLAGS )

if (APPLE)
  set(LAPACK_LIBRARIES ${MKL_LIBRARIES}  TBB::tbb -lc++)
else ()
  set(LAPACK_LIBRARIES ${MKL_LIBRARIES}  TBB::tbb)
endif ()
set(LAPACK_INCLUDE_DIRS ${MKL_INCLUDE_DIRS})
set(LAPACK_LINKER_FLAGS )

endif (AARCH64)
