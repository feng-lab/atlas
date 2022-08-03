#include "zmemorymappedfile.h"

#include "zlog.h"
#include <QFileInfo>
#include <QDir>

namespace nim {

ZMemoryMappedFile::ZMemoryMappedFile(QString filename)
{
  QFileInfo fi(filename);
  auto mmfResult = llfio::mapped_file(llfio::path_handle::path(qUtf8Printable(fi.path())).value(),
                                      qUtf8Printable(fi.fileName()),
                                      llfio::mapped_file_handle::mode::read,
                                      llfio::mapped_file_handle::creation::open_existing,
                                      llfio::mapped_file_handle::caching::all,
                                      llfio::mapped_file_handle::flag::disable_prefetching
  );
  if (mmfResult.has_value()) {
    m_mappedFileHandle = std::move(mmfResult.value());
    m_mappedFileHandleIsValid = true;
    LOG(INFO) << "created memory mapped file for " << filename;
  }
}

void ZMemoryMappedFile::readToBuffer(size_t offset, size_t length, void* buffer)
{
  if (m_mappedFileHandleIsValid) {
    memcpy(buffer, m_mappedFileHandle.address() + offset, length);
  }
}

} // namespace nim
