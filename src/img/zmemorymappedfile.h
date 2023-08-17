#pragma once

#include "zlog.h"
#ifdef ZIMG_USE_LLFIO
#include <llfio.hpp>
#endif

namespace nim {

#ifdef ZIMG_USE_LLFIO

namespace {
namespace llfio = LLFIO_V2_NAMESPACE;
}

class ZMemoryMappedFile
{
public:
  explicit ZMemoryMappedFile(const QString& filename);

  inline void readToBuffer(size_t offset, size_t length, void* buffer) const
  {
    CHECK(m_mappedFileHandleIsValid);
    memcpy(buffer, m_mappedFileHandle.address() + offset, length);
  }

  inline void prefetch(size_t offset, size_t length) const
  {
    auto result =
      llfio::map_handle::prefetch(llfio::map_handle::buffer_type(m_mappedFileHandle.address() + offset, length));
    if (!result.has_value()) {
      LOG(ERROR) << "error prefetching " << result.error().message();
    }
  }

  inline bool isValid() const
  {
    return m_mappedFileHandleIsValid;
  }

private:
  llfio::mapped_file_handle m_mappedFileHandle;
  bool m_mappedFileHandleIsValid = false;
};

#else

class ZMemoryMappedFile
{
public:
  explicit ZMemoryMappedFile(const QString&) {}

  inline void readToBuffer(size_t, size_t, void*) const {}

  inline void prefetch(size_t, size_t) const {}
};

#endif

} // namespace nim
