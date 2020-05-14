#include "zregionannotationpack.h"

#include "zregionannotationdoc.h"
#include <QFileInfo>
#include <QInputDialog>
#include <QApplication>

namespace nim {

ZRegionAnnotationPack::ZRegionAnnotationPack(ZRegionAnnotation* ra, const QString& path, size_t id,
                                             ZRegionAnnotationDoc& doc, QObject* parent)
  : ZObjPack(id, &doc, parent)
  , m_regionAnnotation(ra)
  , m_path(QFileInfo(path).canonicalFilePath())
  , m_doc(doc)
{
  updateDerivedData();
  updatePtsAndSelectedPuncta();
  createContextMenu();
  connect(undoStack(), &QUndoStack::cleanChanged,
          this, &ZRegionAnnotationPack::undoStackCleanChanged);
  if (m_path.isEmpty() && path.endsWith("_anno")) {
    m_path = path;
  }
  if (m_name.isEmpty()) {
    static size_t num = 1;
    m_name = QString("Unsaved RegionAnnotation %1").arg(num++);
  }
}

ZRegionAnnotationPack::~ZRegionAnnotationPack()
{
  undoStack()->disconnect(this);
}

const QString& ZRegionAnnotationPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("%1 regions").arg(m_regionAnnotation->numRegions());
  }
  return m_info;
}

QMenu& ZRegionAnnotationPack::contextMenu()
{
//  m_deleteSelectedPunctaAction->setEnabled(!m_selectedPuncta.empty());
//  m_transferSelectedPunctaToAnotherFileAction->setEnabled(!m_selectedPuncta.empty() && m_doc.objs().size() > 1);
//  m_mergeSelectedPuntaAction->setEnabled(m_selectedPuncta.size() > 1);
//  m_splitSelectedPunctumAction->setEnabled(m_selectedPuncta.size() == 1);
  return m_contextMenu;
}

void ZRegionAnnotationPack::save(const QString& fileName)
{
  m_regionAnnotation->save(fileName);
  m_path = QFileInfo(fileName).canonicalFilePath();
  undoStack()->setClean();
  updateDerivedData();
}

void ZRegionAnnotationPack::updateDerivedData()
{
  m_info.clear();
  if (!m_name.startsWith("Unsaved RegionAnnotation ")) {
    m_name = QFileInfo(m_path).fileName();
  }
  m_tooltip = m_path;
}

void ZRegionAnnotationPack::updatePtsAndSelectedPuncta()
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

void ZRegionAnnotationPack::createContextMenu()
{
//  m_deleteSelectedPunctaAction = new QAction(tr("Delete Selected Puncta"), this);
//  m_deleteSelectedPunctaAction->setStatusTip(tr("Delete all selected puncta"));
//  connect(m_deleteSelectedPunctaAction, &QAction::triggered, this, &ZRegionAnnotationPack::deleteSelectedPuncta);
//
//  m_transferSelectedPunctaToAnotherFileAction = new QAction(tr("Transfer Selected Puncta to Another File..."), this);
//  m_transferSelectedPunctaToAnotherFileAction->setStatusTip(tr("Transfer the selected puncta to another puncta file"));
//  connect(m_transferSelectedPunctaToAnotherFileAction, &QAction::triggered,
//          this, &ZRegionAnnotationPack::transferSelectedPuncta);
//
//  m_mergeSelectedPuntaAction = new QAction(tr("Merge Selected Puncta"), this);
//  m_mergeSelectedPuntaAction->setStatusTip(tr("Merge selected puncta into 1 punctum"));
//  connect(m_mergeSelectedPuntaAction, &QAction::triggered, this, &ZRegionAnnotationPack::mergeSelectedPuncta);
//
//  m_splitSelectedPunctumAction = new QAction(tr("Split Punctum..."), this);
//  m_splitSelectedPunctumAction->setStatusTip(tr("Split the selected punctum into several parts using GMM"));
//  connect(m_splitSelectedPunctumAction, &QAction::triggered, this, &ZRegionAnnotationPack::splitSelectedPunctum);
//
//  m_contextMenu.addAction(m_deleteSelectedPunctaAction);
//  m_contextMenu.addAction(m_transferSelectedPunctaToAnotherFileAction);
//  m_contextMenu.addAction(m_mergeSelectedPuntaAction);
//  m_contextMenu.addAction(m_splitSelectedPunctumAction);
}

} // namespace nim






