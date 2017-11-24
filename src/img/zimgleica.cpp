#include "zimgleica.h"

#include <QUuid>
#include <QXmlStreamReader>

namespace nim {

static_assert(sizeof(QUuid) == 16 && std::is_trivially_copyable<QUuid>::value, "wrong uuid type");

bool ZImgLeica::supportRead() const
{
  return true;
}

bool ZImgLeica::supportWrite() const
{
  return false;
}

QString ZImgLeica::shortName() const
{
  return "Leica Image";
}

QString ZImgLeica::fullName() const
{
  return "Leica Image";
}

QStringList ZImgLeica::extensions() const
{
  QStringList res;
  res << "lif" << "lof";
  return res;
}

void ZImgLeica::readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                         std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                         std::vector<std::set<size_t>>* pyramidalRatios)
{

}

void ZImgLeica::readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene)
{

}

void
ZImgLeica::readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene)
{

}

void ZImgLeica::readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene, size_t ratio)
{

}

} // namespace nim
