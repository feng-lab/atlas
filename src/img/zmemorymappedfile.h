#pragma once

#include "zlog.h"
#include <llfio.hpp>
#include <QString>

namespace nim {

namespace
{
namespace llfio = LLFIO_V2_NAMESPACE;
}

class ZMemoryMappedFile
{
public:
  explicit ZMemoryMappedFile(QString filename);

  inline void readToBuffer(size_t offset, size_t length, void* buffer)
  {
    CHECK(m_mappedFileHandleIsValid);
    memcpy(buffer, m_mappedFileHandle.address() + offset, length);
  }

  inline bool isValid() const
  {
    return m_mappedFileHandleIsValid;
  }

private:
  llfio::mapped_file_handle m_mappedFileHandle;
  bool m_mappedFileHandleIsValid = false;
};

} // namespace nim
