#include "zpuncta.h"

#include "zpunctaio.h"

namespace nim {

ZPuncta::ZPuncta(const QString& filename)
{
  load(filename);
}

bool ZPuncta::canReadFile(const QString& filename)
{
  return ZPunctaIOInstance.canReadFile(filename);
}

bool ZPuncta::canWriteFile(const QString& filename)
{
  return ZPunctaIOInstance.canWriteFile(filename);
}

const QString& ZPuncta::getQtReadNameFilter()
{
  return ZPunctaIOInstance.getQtReadNameFilter();
}

void ZPuncta::getQtWriteNameFilter(QStringList& filters, QStringList& formats)
{
  ZPunctaIOInstance.getQtWriteNameFilter(filters, formats);
}

void ZPuncta::load(const QString& filename)
{
  ZPunctaIOInstance.load(filename, *this);
}

void ZPuncta::save(const QString& filename, const QString& format) const
{
  ZPunctaIOInstance.save(*this, filename, format);
}

} // namespace nim
