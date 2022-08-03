#include "zmemorymappedfilecache.h"

namespace nim {

ZMemoryMappedFileCache& ZMemoryMappedFileCache::instance()
{
  static ZMemoryMappedFileCache cache;
  return cache;
}

ZMemoryMappedFile* ZMemoryMappedFileCache::getMemoryMappedFile(const QString& filename) const
{
  if (auto it = m_mmfs.find(filename); it != m_mmfs.end()) {
    return it->second.get();
  }
  return nullptr;
}

ZMemoryMappedFile* ZMemoryMappedFileCache::getOrCreateMemoryMappedFile(const QString& filename)
{
  if (auto it = m_mmfs.find(filename); it != m_mmfs.end()) {
    return it->second.get();
  } else {
    auto mmfp = std::make_unique<ZMemoryMappedFile>(filename);
    if (mmfp->isValid()) {
      m_mmfs.insert({filename, std::move(mmfp)});
    } else {
      m_mmfs.insert({filename, std::unique_ptr<ZMemoryMappedFile>()});
    }
  }
  return nullptr;
}

} // namespace nim
