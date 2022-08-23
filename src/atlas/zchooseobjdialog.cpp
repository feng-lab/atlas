#include "zchooseobjdialog.h"

#include "zobjdoc.h"
#include <QPushButton>
#include <QLabel>
#include <QDialogButtonBox>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QDir>

namespace nim {

ZChooseObjDialog::ZChooseObjDialog(const ZObjDoc& objDoc, bool multipleSelection, QWidget* parent)
  : QDialog(parent)
  , m_objDoc(&objDoc)
  , m_multipleSelection(multipleSelection)
{
  setWindowTitle(QString("Choose %1").arg(m_objDoc->typeName()));
  createWidget();
  auto objs = m_objDoc->objs();

  bool firstObj = true;
  for (auto id : objs) {
    auto item =
      new QTreeWidgetItem(m_treeWidget,
                          QStringList() << m_objDoc->objName(id) << QDir::toNativeSeparators(m_objDoc->objPath(id)));
    item->setData(0, Qt::UserRole, QVariant::fromValue(id));
    item->setSelected(firstObj && !m_multipleSelection);
    firstObj = false;
  }

  m_treeWidget->resizeColumnToContents(0);
#ifdef __APPLE__
  m_treeWidget->setAlternatingRowColors(true);
#endif
}

ZChooseObjDialog::ZChooseObjDialog(const ZDoc& doc, bool multipleSelection, QWidget* parent)
  : QDialog(parent)
  , m_doc(&doc)
  , m_multipleSelection(multipleSelection)
{
  setWindowTitle(QString("Choose object"));
  createWidget();
  auto objs = m_doc->objs();

  bool firstObj = true;
  for (auto id : objs) {
    auto objDoc = m_doc->idToDoc(id);
    auto item =
      new QTreeWidgetItem(m_treeWidget,
                          QStringList() << objDoc->objName(id) << QDir::toNativeSeparators(objDoc->objPath(id)));
    item->setData(0, Qt::UserRole, QVariant::fromValue(id));
    item->setSelected(firstObj && !m_multipleSelection);
    firstObj = false;
  }

  m_treeWidget->resizeColumnToContents(0);
#ifdef __APPLE__
  m_treeWidget->setAlternatingRowColors(true);
#endif
}

void ZChooseObjDialog::createWidget()
{
  auto lo = new QVBoxLayout(this);
  if (m_objDoc) {
    if (m_multipleSelection) {
      m_label = new QLabel(QString("Choose some %1:").arg(m_objDoc->typeName()), this);
    } else {
      m_label = new QLabel(QString("Choose one %1:").arg(m_objDoc->typeName()), this);
    }
  } else if (m_doc) {
    if (m_multipleSelection) {
      m_label = new QLabel(QString("Choose some objects"), this);
    } else {
      m_label = new QLabel(QString("Choose one object"), this);
    }
  }
  m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_treeWidget = new QTreeWidget(this);
  m_treeWidget->setColumnCount(2);
  m_treeWidget->setHeaderHidden(true);
  m_treeWidget->setItemsExpandable(true);
  m_treeWidget->setMinimumWidth(500);
  m_treeWidget->setSelectionMode(m_multipleSelection ? QAbstractItemView::ExtendedSelection
                                                     : QAbstractItemView::SingleSelection);
  connect(m_treeWidget, &QTreeWidget::itemSelectionChanged, this, &ZChooseObjDialog::updateSelectedIDs);
  m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

  lo->addWidget(m_label);
  lo->addWidget(m_treeWidget);
  lo->addWidget(m_buttonBox);

  m_treeWidget->setFocus();

  connect(m_buttonBox->button(QDialogButtonBox::Ok), &QPushButton::clicked, this, &ZChooseObjDialog::accept);
  connect(m_buttonBox->button(QDialogButtonBox::Cancel), &QPushButton::clicked, this, &ZChooseObjDialog::reject);
}

void ZChooseObjDialog::updateSelectedIDs()
{
  m_selectedIDs.clear();
  for (auto item : m_treeWidget->selectedItems()) {
    m_selectedIDs.push_back(item->data(0, Qt::UserRole).value<size_t>());
  }
}

} // namespace nim
