#include "zmemorymappedfile.h"

#include <QFileInfo>
#include <QDir>

DEFINE_string(
  zimg_llfio_mapped_file_handle_flags,
  "multiplexable",
  "comma-separated list of flags for llfio mapped file handle, default is multiplexable, possible values are: "
  "none,unlink_on_first_close,disable_safety_barriers,disable_safety_unlinks,disable_prefetching,"
  "maximum_prefetching,win_disable_unlink_emulation,win_disable_sparse_file_creation,disable_parallelism,"
  "win_create_case_sensitive_directory,multiplexable,byte_lock_insanity,anonymous_inode");

#ifdef ZIMG_USE_LLFIO

namespace nim {

ZMemoryMappedFile::ZMemoryMappedFile(const QString& filename)
{
  LOG(INFO) << "mapped file handle flags: " << FLAGS_zimg_llfio_mapped_file_handle_flags;
  llfio::mapped_file_handle::flag flag = llfio::mapped_file_handle::flag::none;
  if (QString flagString = QString::fromStdString(FLAGS_zimg_llfio_mapped_file_handle_flags);
      flagString.contains("unlink_on_first_close")) {
    flag |= llfio::mapped_file_handle::flag::unlink_on_first_close;
  } else if (flagString.contains("disable_safety_barriers")) {
    flag |= llfio::mapped_file_handle::flag::disable_safety_barriers;
  } else if (flagString.contains("disable_safety_unlinks")) {
    flag |= llfio::mapped_file_handle::flag::disable_safety_unlinks;
  } else if (flagString.contains("disable_prefetching")) {
    flag |= llfio::mapped_file_handle::flag::disable_prefetching;
  } else if (flagString.contains("maximum_prefetching")) {
    flag |= llfio::mapped_file_handle::flag::maximum_prefetching;
  } else if (flagString.contains("win_disable_unlink_emulation")) {
    flag |= llfio::mapped_file_handle::flag::win_disable_unlink_emulation;
  } else if (flagString.contains("win_disable_sparse_file_creation")) {
    flag |= llfio::mapped_file_handle::flag::win_disable_sparse_file_creation;
  } else if (flagString.contains("disable_parallelism")) {
    flag |= llfio::mapped_file_handle::flag::disable_parallelism;
  } else if (flagString.contains("win_create_case_sensitive_directory")) {
    flag |= llfio::mapped_file_handle::flag::win_create_case_sensitive_directory;
  } else if (flagString.contains("multiplexable")) {
    flag |= llfio::mapped_file_handle::flag::multiplexable;
  } else if (flagString.contains("byte_lock_insanity")) {
    flag |= llfio::mapped_file_handle::flag::byte_lock_insanity;
  } else if (flagString.contains("anonymous_inode")) {
    flag |= llfio::mapped_file_handle::flag::anonymous_inode;
  }
  QFileInfo fi(filename);
  auto mmfResult = llfio::mapped_file(llfio::path_handle::path(qUtf8Printable(fi.path())).value(),
                                      qUtf8Printable(fi.fileName()),
                                      llfio::mapped_file_handle::mode::read,
                                      llfio::mapped_file_handle::creation::open_existing,
                                      llfio::mapped_file_handle::caching::all,
                                      flag);
  if (mmfResult.has_value() && mmfResult.value().is_valid()) {
    m_mappedFileHandle = std::move(mmfResult.value());
    m_mappedFileHandleIsValid = true;
    LOG(INFO) << "created memory mapped file for " << filename;
  } else {
    LOG(ERROR) << "error creating memory mapped file for " << filename << ": " << mmfResult.error().message();
  }
}

} // namespace nim

#endif
