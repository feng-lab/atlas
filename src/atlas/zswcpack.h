#pragma once

#include "zobjpack.h"
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

class ZSwcPack : public ZObjPack
{
Q_OBJECT
public:
  ZSwcPack(ZSwc swc, const QString& path, size_t id, ZSwcDoc& pd, QObject* parent = nullptr);

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

  inline const std::vector<ZSwc::ConstIterator>& rootNodes() const
  { return m_rootNodes; }

  inline const std::map<ZSwc::ConstIterator, std::vector<ZSwc::ConstIterator>>& rootToChildrenNodes() const
  { return m_rootToChildrenNodes; }

  inline const std::set<ZSwc::ConstIterator>& selectedNodes() const
  { return m_selectedNodes; }

  void setSelectedNodes(const std::set<ZSwc::ConstIterator>& sn);

  std::tuple<int, int> getParentRowAndRowOfNode(const ZSwc::ConstIterator& node) const;

  ZSwc::ConstIterator getNodeOfParentRowAndRow(const std::tuple<int, int>& prar) const;

protected:

  void updateViewRelatedData();

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
  // for views
  std::vector<ZSwc::ConstIterator> m_rootNodes;
  std::map<ZSwc::ConstIterator, std::vector<ZSwc::ConstIterator>> m_rootToChildrenNodes;
  std::set<ZSwc::ConstIterator> m_selectedNodes;
};

} // namespace nim
