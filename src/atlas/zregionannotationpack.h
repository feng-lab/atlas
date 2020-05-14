#pragma once

#include "zobjpack.h"
#include "zregionannotation.h"
#include "zglmutils.h"
#include "zbbox.h"
#include <QUndoStack>
#include <QMenu>
#include <QAction>
#include <utility>
#include <vector>
#include <set>

namespace nim {

class ZRegionAnnotationDoc;

class ZRegionAnnotationPack : public ZObjPack
{
Q_OBJECT
public:
  ZRegionAnnotationPack(ZRegionAnnotation* roi, const QString& path,
                        size_t id, ZRegionAnnotationDoc& pd, QObject* parent = nullptr);

  ~ZRegionAnnotationPack() override;

  void updateDerivedData();

  const QString& info() const;

  inline const QString& name() const
  { return m_name; }

  inline const QString& tooltip() const
  { return m_tooltip; }

  inline const QString& path() const
  { return m_path; }

  QUndoStack* undoStack()
  { return m_regionAnnotation->undoStack(); }

  QMenu& contextMenu();

  void save(const QString& fileName);

  // void setSelectedPuncta(const std::set<const ZPunctum*>& sp);

  inline const ZRegionAnnotation& regionAnnotation() const
  { return *m_regionAnnotation; }

  inline ZRegionAnnotation& regionAnnotation()
  { return *m_regionAnnotation; }

  ZBBox<glm::ivec4> boundBox() const
  { return m_regionAnnotation->boundBox(); }

protected:

  void updatePtsAndSelectedPuncta();

  void createContextMenu();

signals:

  void selectionChanged();

  void ROIChanged();

  void undoStackCleanChanged(bool clean);

protected:
  std::unique_ptr<ZRegionAnnotation> m_regionAnnotation;
  QString m_path;
  ZRegionAnnotationDoc& m_doc;
  // QUndoStack m_undoStack;

  QMenu m_contextMenu;

  // derived data
private:
  mutable QString m_info;
  QString m_name;
  QString m_tooltip;
};

} // namespace nim




