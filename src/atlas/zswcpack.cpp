#include "zswcpack.h"

#include "zswcdoc.h"
#include <QFileInfo>
#include <QInputDialog>
#include <QApplication>

namespace nim {

ZSwcPack::ZSwcPack(ZSwc swc, const QString& path, size_t id, ZSwcDoc& doc, QObject* parent)
  : ZObjPack(id, &doc, parent)
  , m_swc(std::move(swc))
  , m_path(QFileInfo(path).canonicalFilePath())
  , m_doc(doc)
{
  updateDerivedData();
  updateViewRelatedData();
  createContextMenu();
  connect(&m_undoStack, &QUndoStack::cleanChanged,
          this, &ZSwcPack::undoStackCleanChanged);
}

ZSwcPack::~ZSwcPack()
{
  m_undoStack.disconnect(this);
}

const QString& ZSwcPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("size %1").arg(m_swc.size());
  }
  return m_info;
}

QMenu& ZSwcPack::contextMenu()
{
//  m_deleteSelectedPunctaAction->setEnabled(!m_selectedPuncta.empty());
//  m_transferSelectedPunctaToAnotherFileAction->setEnabled(!m_selectedPuncta.empty() && m_doc.objs().size() > 1);
//  m_mergeSelectedPuntaAction->setEnabled(m_selectedPuncta.size() > 1);
//  m_splitSelectedPunctumAction->setEnabled(m_selectedPuncta.size() == 1);
  return m_contextMenu;
}

void ZSwcPack::save(const QString &fileName)
{
  m_swc.resortID();
  m_swc.save(fileName);
  m_path = QFileInfo(fileName).canonicalFilePath();
  m_undoStack.setClean();
  updateDerivedData();
}

ZBBox<glm::ivec4> ZSwcPack::boundBox() const
{
  ZBBox<glm::ivec4> res;
  for (auto& p : m_swc) {
    res.expand(glm::ivec4(p.x - p.radius, p.y - p.radius, std::floor(p.z), 0));
    res.expand(glm::ivec4(p.x + p.radius, p.y + p.radius, std::ceil(p.z), 0));
  }
  return res;
}

void ZSwcPack::setSelectedNodes(const std::set<ZSwc::ConstIterator>& sn)
{
  if (m_selectedNodes == sn) {
    return;
  }
  m_selectedNodes = sn;
  for (auto it = m_swc.begin(); it != m_swc.end(); ++it) {
    it->selected = m_selectedNodes.find(it) != m_selectedNodes.end();
  }
  emit selectionChanged();
}

std::tuple<int, int> ZSwcPack::getParentRowAndRowOfNode(const ZSwc::ConstIterator& node) const
{
  CHECK(!ZSwc::isNull(node));
  if (ZSwc::isRoot(node)) {
    for (size_t r = 0; r < m_rootNodes.size(); ++r) {
      if (node == m_rootNodes[r]) {
        return std::make_tuple(static_cast<int>(r), -1);
      }
    }
    CHECK(false);
  } else {
    auto pit = ZSwc::parent(node);
    while (!ZSwc::isRoot(pit)) {
      pit = ZSwc::parent(pit);
    }
    auto it = std::find(m_rootNodes.begin(), m_rootNodes.end(), pit);
    CHECK(it != m_rootNodes.end());
    int parentRow = static_cast<int>(std::distance(m_rootNodes.begin(), it));
    auto& children = m_rootToChildrenNodes.at(m_rootNodes[parentRow]);
    it = std::find(children.begin(), children.end(), node);
    CHECK(it != children.end());
    int row = static_cast<int>(std::distance(children.begin(), it));
    return std::make_tuple(parentRow, row);
  }
}

ZSwc::ConstIterator ZSwcPack::getNodeOfParentRowAndRow(const std::tuple<int, int>& prar) const
{
  auto [parentRow, row] = prar;
  CHECK(parentRow >= 0) << parentRow;
  if (row < 0) {
    return m_rootNodes[parentRow];
  } else {
    return m_rootToChildrenNodes.at(m_rootNodes[parentRow])[row];
  }
}

void ZSwcPack::updateDerivedData()
{
  m_info.clear();
  m_name = QFileInfo(m_path).fileName();
  m_tooltip = m_path;
}

void ZSwcPack::updateViewRelatedData()
{
  m_rootNodes.clear();
  m_rootToChildrenNodes.clear();
  m_selectedNodes.clear();
  for (auto rit = m_swc.cbeginRoot(); rit != m_swc.cendRoot(); ++rit) {
    m_rootNodes.emplace_back(rit);
  }
  for (auto& rit : m_rootNodes) {
    m_rootToChildrenNodes[rit].clear();
    auto it = m_swc.cbegin(rit);
    if (it->selected) {
      m_selectedNodes.insert(it);
    }
    CHECK(it == rit);
    for (++it; it != m_swc.cend(rit); ++it) {
      m_rootToChildrenNodes[rit].push_back(it);
      if (it->selected) {
        m_selectedNodes.insert(it);
      }
    }
  }
}

void ZSwcPack::createContextMenu()
{
//  m_deleteSelectedPunctaAction = new QAction(tr("Delete Selected Puncta"), this);
//  m_deleteSelectedPunctaAction->setStatusTip(tr("Delete all selected puncta"));
//  connect(m_deleteSelectedPunctaAction, &QAction::triggered, this, &ZSwcPack::deleteSelectedPuncta);
//
//  m_transferSelectedPunctaToAnotherFileAction = new QAction(tr("Transfer Selected Puncta to Another File..."), this);
//  m_transferSelectedPunctaToAnotherFileAction->setStatusTip(tr("Transfer the selected puncta to another puncta file"));
//  connect(m_transferSelectedPunctaToAnotherFileAction, &QAction::triggered,
//          this, &ZSwcPack::transferSelectedPuncta);
//
//  m_mergeSelectedPuntaAction = new QAction(tr("Merge Selected Puncta"), this);
//  m_mergeSelectedPuntaAction->setStatusTip(tr("Merge selected puncta into 1 punctum"));
//  connect(m_mergeSelectedPuntaAction, &QAction::triggered, this, &ZSwcPack::mergeSelectedPuncta);
//
//  m_splitSelectedPunctumAction = new QAction(tr("Split Punctum..."), this);
//  m_splitSelectedPunctumAction->setStatusTip(tr("Split the selected punctum into several parts using GMM"));
//  connect(m_splitSelectedPunctumAction, &QAction::triggered, this, &ZSwcPack::splitSelectedPunctum);
//
//  m_contextMenu.addAction(m_deleteSelectedPunctaAction);
//  m_contextMenu.addAction(m_transferSelectedPunctaToAnotherFileAction);
//  m_contextMenu.addAction(m_mergeSelectedPuntaAction);
//  m_contextMenu.addAction(m_splitSelectedPunctumAction);
}

} // namespace nim


