#include "zpunctapack.h"

#include "zpunctadoc.h"
#include <QFileInfo>
#include <QInputDialog>
#include <QApplication>

namespace nim {

ZPunctaPack::ZPunctaPack(ZPuncta puncta, const QString& path, size_t id, ZPunctaDoc& doc, QObject* parent)
  : ZObjPack(id, &doc, parent)
  , m_puncta(std::move(puncta))
  , m_path(QFileInfo(path).canonicalFilePath())
  , m_doc(doc)
{
  updateDerivedData();
  updateViewRelatedData();
  createContextMenu();
  connect(&m_undoStack, &QUndoStack::cleanChanged, this, &ZPunctaPack::undoStackCleanChanged);
}

ZPunctaPack::~ZPunctaPack()
{
  m_undoStack.disconnect(this);
}

const QString& ZPunctaPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("%1 puncta").arg(m_puncta.data.size());
  }
  return m_info;
}

QMenu& ZPunctaPack::contextMenu()
{
  m_deleteSelectedPunctaAction->setEnabled(!m_selectedPuncta.empty());
  m_transferSelectedPunctaToAnotherFileAction->setEnabled(!m_selectedPuncta.empty() && m_doc.objs().size() > 1);
  m_mergeSelectedPuntaAction->setEnabled(m_selectedPuncta.size() > 1);
  m_splitSelectedPunctumAction->setEnabled(m_selectedPuncta.size() == 1);
  return m_contextMenu;
}

void ZPunctaPack::save(const QString& fileName, const QString& format)
{
  m_puncta.save(fileName, format);
  m_path = QFileInfo(fileName).canonicalFilePath();
  m_undoStack.setClean();
  updateDerivedData();
}

void ZPunctaPack::setSelectedPuncta(const std::set<const ZPunctum*>& sp)
{
  if (m_selectedPuncta == sp) {
    return;
  }
  m_selectedPuncta = sp;
  for (auto& mp : m_puncta.data) {
    mp.setSelected(m_selectedPuncta.contains(&mp));
  }
  Q_EMIT selectionChanged();
}

void ZPunctaPack::onPunctumSelected(const ZPunctum* p, bool append)
{
  if (append) {
    if (!p) {
      return;
    }
    auto it = m_selectedPuncta.find(p);
    if (it == m_selectedPuncta.end()) {
      const_cast<ZPunctum*>(p)->setSelected(true);
      m_selectedPuncta.insert(p);
      Q_EMIT selectionChanged();
    } else {
      const_cast<ZPunctum*>(p)->setSelected(false);
      m_selectedPuncta.erase(it);
      Q_EMIT selectionChanged();
    }
  } else {
    if (!p && m_selectedPuncta.empty()) {
      return;
    }
    if (p && m_selectedPuncta.size() == 1 && m_selectedPuncta.contains(p)) {
      return;
    }
    for (auto& mp : m_puncta.data) {
      mp.setSelected(&mp == p);
    }
    if (p) {
      m_selectedPuncta = std::set<const ZPunctum*>{p};
    } else {
      m_selectedPuncta.clear();
    }
    Q_EMIT selectionChanged();
  }
}

ZBBox<glm::ivec4> ZPunctaPack::boundBox() const
{
  ZBBox<glm::ivec4> res;
  for (auto& p : m_puncta.data) {
    res.expand(glm::ivec4(p.x() - p.radius(), p.y() - p.radius(), std::floor(p.z()), 0));
    res.expand(glm::ivec4(p.x() + p.radius(), p.y() + p.radius(), std::ceil(p.z()), 0));
  }
  return res;
}

void ZPunctaPack::deleteSelectedPuncta()
{
  if (m_selectedPuncta.empty()) {
    return;
  }
  m_undoStack.push(new ZPunctaEditCommand(QString("Deleted Selected %1 Puncta").arg(m_selectedPuncta.size()),
                                          *this,
                                          m_selectedPuncta));
}

void ZPunctaPack::transferSelectedPuncta()
{
  if (m_selectedPuncta.empty() || m_doc.objs().size() < 2) {
    return;
  }
  auto objID = m_doc.chooseOneObjWithWidget("Transfer Selected Puncta to File", QApplication::activeWindow());
  if (objID > 0) {
    auto& targetPunctaPack = m_doc.punctaPack(objID);
    if (&targetPunctaPack == this) {
      return;
    }
    std::list<ZPunctum> selected;
    for (auto& p : m_selectedPuncta) {
      selected.push_back(*p);
    }
    m_undoStack.push(new ZPunctaEditCommand(QString("Transfer Selected %1 Puncta").arg(m_selectedPuncta.size()),
                                            *this,
                                            m_selectedPuncta));
    targetPunctaPack.m_undoStack.push(
      new ZPunctaEditCommand(QString("Accept Transferred %1 Puncta").arg(selected.size()),
                             targetPunctaPack,
                             std::set<const ZPunctum*>(),
                             selected));
  }
}

void ZPunctaPack::mergeSelectedPuncta()
{
  if (m_selectedPuncta.size() < 2) {
    return;
  }
  std::list<ZPunctum> selected;
  for (auto& p : m_selectedPuncta) {
    selected.push_back(*p);
  }
  auto mergedPunctum = ZPunctum::merge(selected.begin(), selected.end());
  m_undoStack.push(new ZPunctaEditCommand(QString("Merge Selected %1 Puncta").arg(m_selectedPuncta.size()),
                                          *this,
                                          m_selectedPuncta,
                                          std::list<ZPunctum>{mergedPunctum}));
}

void ZPunctaPack::splitSelectedPunctum()
{
  if (m_selectedPuncta.size() != 1) {
    return;
  }
  bool ok = false;
  int numParts = QInputDialog::getInt(QApplication::activeWindow(),
                                      tr("Split Punctum"),
                                      tr("Number of Parts to Split Into:"),
                                      2,
                                      2,
                                      10,
                                      1,
                                      &ok);
  if (ok) {
    auto splitedPuncta = (*m_selectedPuncta.begin())->split(numParts);
    m_undoStack.push(new ZPunctaEditCommand(QString("Merge Selected %1 Puncta").arg(m_selectedPuncta.size()),
                                            *this,
                                            m_selectedPuncta,
                                            splitedPuncta));
  }
}

void ZPunctaPack::showPunctaContextMenu(QPoint globalPos)
{
  contextMenu().popup(globalPos);
}

void ZPunctaPack::updateDerivedData()
{
  m_info.clear();
  m_name = QFileInfo(m_path).fileName();
  m_tooltip = m_path;
}

void ZPunctaPack::updateViewRelatedData()
{
  m_punctaPts.clear();
  m_selectedPuncta.clear();
  m_punctumToRow.clear();
  index_t idx = 0;
  for (const auto& p : m_puncta.data) {
    m_punctaPts.push_back(&p);
    if (p.isSelected()) {
      m_selectedPuncta.insert(&p);
    }
    m_punctumToRow[&p] = idx++;
  }
}

void ZPunctaPack::createContextMenu()
{
  m_deleteSelectedPunctaAction = new QAction(tr("Delete Selected Puncta"), this);
  m_deleteSelectedPunctaAction->setStatusTip(tr("Delete all selected puncta"));
  connect(m_deleteSelectedPunctaAction, &QAction::triggered, this, &ZPunctaPack::deleteSelectedPuncta);

  m_transferSelectedPunctaToAnotherFileAction = new QAction(tr("Transfer Selected Puncta to Another File..."), this);
  m_transferSelectedPunctaToAnotherFileAction->setStatusTip(tr("Transfer the selected puncta to another puncta file"));
  connect(m_transferSelectedPunctaToAnotherFileAction, &QAction::triggered, this, &ZPunctaPack::transferSelectedPuncta);

  m_mergeSelectedPuntaAction = new QAction(tr("Merge Selected Puncta"), this);
  m_mergeSelectedPuntaAction->setStatusTip(tr("Merge selected puncta into 1 punctum"));
  connect(m_mergeSelectedPuntaAction, &QAction::triggered, this, &ZPunctaPack::mergeSelectedPuncta);

  m_splitSelectedPunctumAction = new QAction(tr("Split Punctum..."), this);
  m_splitSelectedPunctumAction->setStatusTip(tr("Split the selected punctum into several parts using GMM"));
  connect(m_splitSelectedPunctumAction, &QAction::triggered, this, &ZPunctaPack::splitSelectedPunctum);

  m_contextMenu.addAction(m_deleteSelectedPunctaAction);
  m_contextMenu.addAction(m_transferSelectedPunctaToAnotherFileAction);
  m_contextMenu.addAction(m_mergeSelectedPuntaAction);
  m_contextMenu.addAction(m_splitSelectedPunctumAction);
}

} // namespace nim
