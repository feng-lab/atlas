#pragma once

#include "zpuncta.h"
#include "zglmutils.h"
#include "zbbox.h"
#include <QUndoStack>
#include <vector>
#include <set>

namespace nim {

class ZPunctaPack : public QObject
{
  Q_OBJECT
public:
  ZPunctaPack(ZPuncta puncta, const QString& path, QObject* parent = nullptr);

  void updateDerivedData();

  const QString& info() const;

  inline const QString& name() const
  { return m_name; }

  inline const QString& tooltip() const
  { return m_tooltip; }

  inline const QString& path() const
  { return m_path; }

  QUndoStack* undoStack()
  { return &m_undoStack; }

  void save(const QString& fileName, const QString& format = "");

  const std::vector<const ZPunctum*>& punctaPts() const
  { return m_punctaPts; }

  const std::set<const ZPunctum*>& selectedPuncta() const
  { return m_selectedPuncta; }

  void setSelectedPuncta(const std::set<const ZPunctum*>& sp);

  const ZPuncta& puncta() const
  { return m_puncta; }

  void onPunctumSelected(const ZPunctum* p, bool append);

  ZBBox<glm::ivec4> boundBox() const;

signals:
  void selectionChanged();

protected:
  ZPuncta m_puncta;
  QString m_path;
  bool m_hasUnsavedChange = false;
  QUndoStack m_undoStack;

  // derived data
private:
  mutable QString m_info;
  QString m_name;
  QString m_tooltip;
  std::vector<const ZPunctum*> m_punctaPts;
  std::set<const ZPunctum*> m_selectedPuncta;
};

} // namespace nim




