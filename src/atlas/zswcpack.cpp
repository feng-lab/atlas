#include "zswcpack.h"

#include "zswcdoc.h"
#include <QFileInfo>
#include <QInputDialog>
#include <QApplication>

namespace nim {

ZSwcPack::ZSwcPack(ZSwc swc, const QString& path, ZSwcDoc& doc, QObject* parent)
  : QObject(parent)
  , m_swc(std::move(swc))
  , m_path(QFileInfo(path).canonicalFilePath())
  , m_doc(doc)
{
  updateDerivedData();
  updatePtsAndSelectedPuncta();
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

void ZSwcPack::updateDerivedData()
{
  m_info.clear();
  m_name = QFileInfo(m_path).fileName();
  m_tooltip = m_path;
}

void ZSwcPack::updatePtsAndSelectedPuncta()
{
//  m_punctaPts.clear();
//  m_selectedPuncta.clear();
//  for (const auto& p : m_puncta) {
//    m_punctaPts.push_back(&p);
//    if (p.isSelected()) {
//      m_selectedPuncta.insert(&p);
//    }
//  }
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


