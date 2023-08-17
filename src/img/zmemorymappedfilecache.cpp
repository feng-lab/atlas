#include "zmemorymappedfilecache.h"

namespace nim {

ZMemoryMappedFileCache& ZMemoryMappedFileCache::instance()
{
  static ZMemoryMappedFileCache cache;
  return cache;
}

ZMemoryMappedFile* ZMemoryMappedFileCache::getMemoryMappedFile([[maybe_unused]] const QString& filename) const
{
#ifdef ZIMG_USE_LLFIO
  if (auto it = m_mmfs.find(filename); it != m_mmfs.end()) {
    return it->second.get();
  }
#endif
  return nullptr;
}

ZMemoryMappedFile* ZMemoryMappedFileCache::getOrCreateMemoryMappedFile([[maybe_unused]] const QString& filename)
{
#ifdef ZIMG_USE_LLFIO
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
#endif
  return nullptr;
}

} // namespace nim
