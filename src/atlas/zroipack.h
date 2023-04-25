#pragma once

#include "zobjpack.h"
#include "zroi.h"
#include "zglmutils.h"
#include "zbbox.h"
#include <QUndoStack>
#include <QMenu>
#include <QAction>
#include <utility>
#include <vector>
#include <set>

namespace nim {

class ZROIDoc;

class ZROIPack : public ZObjPack
{
  Q_OBJECT

public:
  ZROIPack(ZROI* roi, const QString& path, size_t id, ZROIDoc& pd, QObject* parent = nullptr);

  ~ZROIPack() override;

  void updateDerivedData();

  const QString& info() const;

  inline const QString& name() const
  {
    return m_name;
  }

  inline const QString& tooltip() const
  {
    return m_tooltip;
  }

  inline const QString& path() const
  {
    return m_path;
  }

  QUndoStack* undoStack()
  {
    return m_roi->undoStack();
  }

  void setHasUnsavedChange(bool v)
  {
    m_hasUnsavedChange = v;
  }

  bool hasUnsavedChange() const
  {
    return m_hasUnsavedChange;
  }

  QMenu& contextMenu();

  void save(const QString& fileName);

  // void setSelectedPuncta(const std::set<const ZPunctum*>& sp);

  inline const ZROI& roi() const
  {
    return *m_roi;
  }

  inline ZROI& roi()
  {
    return *m_roi;
  }

  ZBBox<glm::ivec4> boundBox() const
  {
    return m_roi->boundBox();
  }

protected:
  void updatePtsAndSelectedPuncta();

  void createContextMenu();

Q_SIGNALS:
  void selectionChanged();

  void ROIChanged();

  void undoStackCleanChanged(bool clean);

protected:
  std::unique_ptr<ZROI> m_roi;
  QString m_path;
  ZROIDoc& m_doc;
  // QUndoStack m_undoStack;
  bool m_hasUnsavedChange = false;

  QMenu m_contextMenu;

  // derived data

private:
  mutable QString m_info;
  QString m_name;
  QString m_tooltip;
};

} // namespace nim
