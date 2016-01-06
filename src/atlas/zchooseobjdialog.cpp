#include "zchooseobjdialog.h"

#include <QPushButton>
#include <QLabel>
#include <QDialogButtonBox>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QDir>
#include "zobjdoc.h"
#include "zdoc.h"

namespace nim {

ZChooseObjDialog::ZChooseObjDialog(const ZObjDoc &doc, QWidget *parent)
  : QDialog(parent), m_doc(doc), m_selectedID(0)
{
  setWindowTitle(QString("Choose %1").arg(m_doc.typeName()));
  createWidget();
  QList<size_t> objs = m_doc.objs();

  for (int i=0; i<objs.size(); ++i) {
    size_t id = objs[i];
    QTreeWidgetItem *item = new QTreeWidgetItem(m_treeWidget, QStringList()
                                                << m_doc.objName(id) << QDir::toNativeSeparators(m_doc.objPath(id)));
    item->setData(0, Qt::UserRole, qVariantFromValue(id));
    m_treeWidget->setItemSelected(item, i == 0);
  }

  m_treeWidget->resizeColumnToContents(0);
#ifdef __APPLE__
  m_treeWidget->setAlternatingRowColors(true);
#endif
}

void ZChooseObjDialog::createWidget()
{
  QVBoxLayout *lo = new QVBoxLayout(this);
  m_label = new QLabel(QString("Choose one %1:").arg(m_doc.typeName()), this);
  m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_treeWidget = new QTreeWidget(this);
  m_treeWidget->setColumnCount(2);
  m_treeWidget->setHeaderHidden(true);
  m_treeWidget->setItemsExpandable(true);
  m_treeWidget->setMinimumWidth(500);
  m_treeWidget->setSelectionMode(QAbstractItemView::SingleSelection);
  connect(m_treeWidget, SIGNAL(itemSelectionChanged()), this, SLOT(updateSelectedID()));
  m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

  lo->addWidget(m_label);
  lo->addWidget(m_treeWidget);
  lo->addWidget(m_buttonBox);

  m_treeWidget->setFocus();

  connect(m_buttonBox->button(QDialogButtonBox::Ok), SIGNAL(clicked()),
          this, SLOT(accept()));
  connect(m_buttonBox->button(QDialogButtonBox::Cancel), SIGNAL(clicked()),
          this, SLOT(reject()));
}

void ZChooseObjDialog::updateSelectedID()
{
  QList<QTreeWidgetItem*> sis = m_treeWidget->selectedItems();
  if (sis.empty()) {
    m_selectedID = 0;
  } else {
    m_selectedID = sis[0]->data(0, Qt::UserRole).value<size_t>();
  }
}

} // namespace nim

