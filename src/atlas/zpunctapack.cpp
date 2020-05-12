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
    if (p.isSelected()) {
      m_selectedPuncta.insert(&p);
    }
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

void ZPunctaPack::setSelectedPuncta(const std::set<const ZPunctum*>& sp)
{
  if (m_selectedPuncta == sp) {
    return;
  }
  m_selectedPuncta = sp;
  emit selectionChanged();
}

void ZPunctaPack::onPunctumSelected(const ZPunctum* p, bool append)
{
  if (append) {
    if (p && m_selectedPuncta.find(p) == m_selectedPuncta.end()) {
      const_cast<ZPunctum*>(p)->setSelected(true);
      m_selectedPuncta.insert(p);
      emit selectionChanged();
    }
  } else {
    if (!p && m_selectedPuncta.empty()) {
      return;
    }
    if (p && m_selectedPuncta.size() == 1 && m_selectedPuncta.find(p) != m_selectedPuncta.end()) {
      return;
    }
    for (auto& mp : m_puncta) {
      mp.setSelected(&mp == p);
    }
    if (p) {
      m_selectedPuncta = std::set<const ZPunctum*>{p};
    } else {
      m_selectedPuncta.clear();
    }
    emit selectionChanged();
  }
}



} // namespace nim
