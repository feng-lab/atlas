#include "zpunctapack.h"

#include <QFileInfo>

namespace nim {

ZPunctaPack::ZPunctaPack(ZPuncta puncta, const QString& path, QObject* parent)
  : QObject(parent)
  , m_puncta(std::move(puncta))
  , m_path(QFileInfo(path).canonicalFilePath())
{
  updateDerivedData();
  for (const auto& p : m_puncta) {
    m_punctaPts.push_back(&p);
  }
}

void ZPunctaPack::updateDerivedData()
{
  m_info.clear();
  m_name = QFileInfo(m_path).fileName();
  m_tooltip = m_path;
}

const QString& ZPunctaPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("%1 puncta").arg(m_puncta.size());
  }
  return m_info;
}

void ZPunctaPack::save(const QString &fileName, const QString &format)
{
  m_puncta.save(fileName, format);
  m_path = QFileInfo(fileName).canonicalFilePath();
  m_undoStack.setClean();
  m_hasUnsavedChange = false;
  updateDerivedData();
}

} // namespace nim
