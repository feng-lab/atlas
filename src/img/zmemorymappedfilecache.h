#pragma once

#include "zmemorymappedfile.h"
#include "zfolly.h"

namespace nim {

class ZMemoryMappedFileCache
{
public:
  static ZMemoryMappedFileCache& instance();

  // remove copy and move constructors and assign operators
  ZMemoryMappedFileCache(const ZMemoryMappedFileCache&) = delete; // Copy construct
  ZMemoryMappedFileCache(ZMemoryMappedFileCache&&) = delete; // Move construct
  ZMemoryMappedFileCache& operator=(const ZMemoryMappedFileCache&) = delete; // Copy assign
  ZMemoryMappedFileCache& operator=(ZMemoryMappedFileCache&&) = delete; // Move assign

  [[nodiscard]] ZMemoryMappedFile* getMemoryMappedFile(const QString& filename) const;

  ZMemoryMappedFile* getOrCreateMemoryMappedFile(const QString& filename);

protected:
  ZMemoryMappedFileCache() = default;

private:
  folly::ConcurrentHashMap<QString, std::unique_ptr<ZMemoryMappedFile>> m_mmfs;
};

} // namespace nim
