#include "zpuncta.h"

#include "zpunctaio.h"

namespace nim {

ZPuncta::ZPuncta(const QString& filename)
{
  load(filename);
}

ZPuncta::ZPuncta(const std::list<nim::ZPunctum>& p)
{
  m_d = p;
}

bool ZPuncta::canReadFile(const QString& filename)
{
  return ZPunctaIO::instance().canReadFile(filename);
}

bool ZPuncta::canWriteFile(const QString& filename)
{
  return ZPunctaIO::instance().canWriteFile(filename);
}

const QString& ZPuncta::getQtReadNameFilter()
{
  return ZPunctaIO::instance().getQtReadNameFilter();
}

void ZPuncta::getQtWriteNameFilter(QStringList& filters, QStringList& formats)
{
  ZPunctaIO::instance().getQtWriteNameFilter(filters, formats);
}

void ZPuncta::load(const QString& filename)
{
  ZPunctaIO::instance().load(filename, *this);
}

void ZPuncta::save(const QString& filename, const QString& format) const
{
  ZPunctaIO::instance().save(*this, filename, format);
}

} // namespace nim
