#include "zmemorymappedfile.h"
#include "zcommandlineflags.h"

#include <QFileInfo>
#include <QDir>

#include <absl/strings/str_join.h>

#include <string>
#include <vector>

ABSL_FLAG(std::vector<std::string>,
          zimg_llfio_mapped_file_handle_flags,
          std::vector<std::string>{"multiplexable"},
          "comma-separated list of flags for llfio mapped file handle, default is multiplexable, possible values are: "
          "none,unlink_on_first_close,disable_safety_barriers,disable_safety_unlinks,disable_prefetching,"
          "maximum_prefetching,win_disable_unlink_emulation,win_disable_sparse_file_creation,disable_parallelism,"
          "win_create_case_sensitive_directory,multiplexable,byte_lock_insanity,anonymous_inode");

#ifdef ZIMG_USE_LLFIO

namespace nim {
namespace {

struct MappedFileHandleFlagSpec
{
  const char* name;
  llfio::mapped_file_handle::flag value;
};

constexpr MappedFileHandleFlagSpec kMappedFileHandleFlagSpecs[] = {
  {"unlink_on_first_close",               llfio::mapped_file_handle::flag::unlink_on_first_close              },
  {"disable_safety_barriers",             llfio::mapped_file_handle::flag::disable_safety_barriers            },
  {"disable_safety_unlinks",              llfio::mapped_file_handle::flag::disable_safety_unlinks             },
  {"disable_prefetching",                 llfio::mapped_file_handle::flag::disable_prefetching                },
  {"maximum_prefetching",                 llfio::mapped_file_handle::flag::maximum_prefetching                },
  {"win_disable_unlink_emulation",        llfio::mapped_file_handle::flag::win_disable_unlink_emulation       },
  {"win_disable_sparse_file_creation",    llfio::mapped_file_handle::flag::win_disable_sparse_file_creation   },
  {"disable_parallelism",                 llfio::mapped_file_handle::flag::disable_parallelism                },
  {"win_create_case_sensitive_directory", llfio::mapped_file_handle::flag::win_create_case_sensitive_directory},
  {"multiplexable",                       llfio::mapped_file_handle::flag::multiplexable                      },
  {"byte_lock_insanity",                  llfio::mapped_file_handle::flag::byte_lock_insanity                 },
  {"anonymous_inode",                     llfio::mapped_file_handle::flag::anonymous_inode                    },
};

llfio::mapped_file_handle::flag mappedFileHandleFlagsFromNames(const std::vector<std::string>& flagNames)
{
  llfio::mapped_file_handle::flag flags = llfio::mapped_file_handle::flag::none;
  for (const std::string& name : flagNames) {
    if (name.empty() || name == "none") {
      continue;
    }
    bool matched = false;
    for (const auto& spec : kMappedFileHandleFlagSpecs) {
      if (name == spec.name) {
        flags |= spec.value;
        matched = true;
        break;
      }
    }
    if (!matched) {
      LOG(WARNING) << "unknown llfio mapped file handle flag: " << name;
    }
  }
  return flags;
}

} // namespace

ZMemoryMappedFile::ZMemoryMappedFile(const QString& filename)
{
  const std::vector<std::string> flagNames = absl::GetFlag(FLAGS_zimg_llfio_mapped_file_handle_flags);
  LOG(INFO) << "mapped file handle flags: " << absl::StrJoin(flagNames, ",");
  const llfio::mapped_file_handle::flag flag = mappedFileHandleFlagsFromNames(flagNames);
  QFileInfo fi(filename);
  auto mmfResult = llfio::mapped_file(llfio::path_handle::path(fi.path().toStdString()).value(),
                                      fi.fileName().toStdString(),
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
