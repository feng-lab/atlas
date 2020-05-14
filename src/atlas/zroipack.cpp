#include "zroipack.h"

#include "zroidoc.h"
#include <QFileInfo>
#include <QInputDialog>
#include <QApplication>

namespace nim {

ZROIPack::ZROIPack(ZROI* roi, const QString& path, size_t id, ZROIDoc& doc, QObject* parent)
  : ZObjPack(id, &doc, parent)
  , m_roi(roi)
  , m_path(QFileInfo(path).canonicalFilePath())
  , m_doc(doc)
{
  updateDerivedData();
  updatePtsAndSelectedPuncta();
  createContextMenu();
  connect(undoStack(), &QUndoStack::cleanChanged,
          this, &ZROIPack::undoStackCleanChanged);
  if (m_path.isEmpty() && path.endsWith("_roi")) {
    m_path = path;
  }
  if (m_name.isEmpty()) {
    m_hasUnsavedChange = true;
    static size_t num = 1;
    m_name = QString("Unsaved ROI %1").arg(num++);
  }
}

ZROIPack::~ZROIPack()
{
  undoStack()->disconnect(this);
}

const QString& ZROIPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("%1 slices").arg(m_roi->numSlices());
  }
  return m_info;
}

QMenu& ZROIPack::contextMenu()
{
//  m_deleteSelectedPunctaAction->setEnabled(!m_selectedPuncta.empty());
//  m_transferSelectedPunctaToAnotherFileAction->setEnabled(!m_selectedPuncta.empty() && m_doc.objs().size() > 1);
//  m_mergeSelectedPuntaAction->setEnabled(m_selectedPuncta.size() > 1);
//  m_splitSelectedPunctumAction->setEnabled(m_selectedPuncta.size() == 1);
  return m_contextMenu;
}

void ZROIPack::save(const QString &fileName)
{
  m_roi->save(fileName);
  m_path = QFileInfo(fileName).canonicalFilePath();
  undoStack()->setClean();
  m_hasUnsavedChange = false;
  updateDerivedData();
}

void ZROIPack::updateDerivedData()
{
  m_info.clear();
  if (!m_name.startsWith("Unsaved ROI ")) {
    m_name = QFileInfo(m_path).fileName();
  }
  m_tooltip = m_path;
}

void ZROIPack::updatePtsAndSelectedPuncta()
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

void ZROIPack::createContextMenu()
{
//  m_deleteSelectedPunctaAction = new QAction(tr("Delete Selected Puncta"), this);
//  m_deleteSelectedPunctaAction->setStatusTip(tr("Delete all selected puncta"));
//  connect(m_deleteSelectedPunctaAction, &QAction::triggered, this, &ZROIPack::deleteSelectedPuncta);
//
//  m_transferSelectedPunctaToAnotherFileAction = new QAction(tr("Transfer Selected Puncta to Another File..."), this);
//  m_transferSelectedPunctaToAnotherFileAction->setStatusTip(tr("Transfer the selected puncta to another puncta file"));
//  connect(m_transferSelectedPunctaToAnotherFileAction, &QAction::triggered,
//          this, &ZROIPack::transferSelectedPuncta);
//
//  m_mergeSelectedPuntaAction = new QAction(tr("Merge Selected Puncta"), this);
//  m_mergeSelectedPuntaAction->setStatusTip(tr("Merge selected puncta into 1 punctum"));
//  connect(m_mergeSelectedPuntaAction, &QAction::triggered, this, &ZROIPack::mergeSelectedPuncta);
//
//  m_splitSelectedPunctumAction = new QAction(tr("Split Punctum..."), this);
//  m_splitSelectedPunctumAction->setStatusTip(tr("Split the selected punctum into several parts using GMM"));
//  connect(m_splitSelectedPunctumAction, &QAction::triggered, this, &ZROIPack::splitSelectedPunctum);
//
//  m_contextMenu.addAction(m_deleteSelectedPunctaAction);
//  m_contextMenu.addAction(m_transferSelectedPunctaToAnotherFileAction);
//  m_contextMenu.addAction(m_mergeSelectedPuntaAction);
//  m_contextMenu.addAction(m_splitSelectedPunctumAction);
}

} // namespace nim




