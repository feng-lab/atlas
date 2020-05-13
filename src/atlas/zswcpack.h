#pragma once

#include "zswc.h"
#include "zglmutils.h"
#include "zbbox.h"
#include <QUndoStack>
#include <QMenu>
#include <QAction>
#include <utility>
#include <vector>
#include <set>

namespace nim {

class ZSwcDoc;

class ZSwcPack : public QObject
{
Q_OBJECT
public:
  ZSwcPack(ZSwc swc, const QString& path, ZSwcDoc& pd, QObject* parent = nullptr);

  ~ZSwcPack() override;

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

  QMenu& contextMenu();

  void save(const QString& fileName);

  // void setSelectedPuncta(const std::set<const ZPunctum*>& sp);

  inline const ZSwc& swc() const
  { return m_swc; }

  ZBBox<glm::ivec4> boundBox() const;

protected:

  void updatePtsAndSelectedPuncta();

  void createContextMenu();

signals:

  void selectionChanged();

  void swcChanged();

  void undoStackCleanChanged(bool clean);

protected:
  ZSwc m_swc;
  QString m_path;
  ZSwcDoc& m_doc;
  QUndoStack m_undoStack;

  QMenu m_contextMenu;

  // derived data
private:
  mutable QString m_info;
  QString m_name;
  QString m_tooltip;
};

} // namespace nim
