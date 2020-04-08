#include "zchooseregiondialog.h"

#include "zregionannotation.h"
#include <QPushButton>
#include <QLabel>
#include <QDialogButtonBox>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QDir>

namespace nim {

ZChooseRegionDialog::ZChooseRegionDialog(const ZRegionAnnotation& ra, QWidget* parent)
  : QDialog(parent), m_regionAnnotation(ra)
{
  setWindowTitle(QString("Choose Region"));
  createWidget();
  const auto& annotationTree = m_regionAnnotation.annotationTree();

  std::map<int64_t, QTreeWidgetItem*> idToItem;
  for (auto it = annotationTree.cbegin(); it != annotationTree.cend(); ++it) {
    const auto& node = *it;
    if (annotationTree.isRoot(it)) {
      auto item = new QTreeWidgetItem(m_treeWidget,
                                      QStringList() << node.abbreviation << node.name << QString::number(node.id)
                                                    << "" << "" << "");
      idToItem[node.id] = item;
      item->setData(0, Qt::UserRole, QVariant::fromValue(node.id));
      item->setSelected(false);
    } else {
      const auto& parentNode = *annotationTree.parent(it);
      auto item = new QTreeWidgetItem(idToItem[parentNode.id],
                                      QStringList() << node.abbreviation << node.name << QString::number(node.id)
                                                    << parentNode.abbreviation << parentNode.name << QString::number(parentNode.id));
      idToItem[node.id] = item;
      item->setData(0, Qt::UserRole, QVariant::fromValue(node.id));
      item->setSelected(false);
    }
  }

  m_treeWidget->expandAll();
  m_treeWidget->resizeColumnToContents(0);
#ifdef __APPLE__
  m_treeWidget->setAlternatingRowColors(true);
#endif
}

void ZChooseRegionDialog::createWidget()
{
  auto lo = new QVBoxLayout(this);
  m_label = new QLabel(QString("Choose one region"), this);
  m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_treeWidget = new QTreeWidget(this);
  QStringList headerLabels;
  headerLabels.push_back(tr("acronym"));
  headerLabels.push_back(tr("name"));
  headerLabels.push_back(tr("id"));
  headerLabels.push_back(tr("parent_acronym"));
  headerLabels.push_back(tr("parent_name"));
  headerLabels.push_back(tr("parent_id"));
  m_treeWidget->setColumnCount(headerLabels.count());
  m_treeWidget->setHeaderLabels(headerLabels);
  m_treeWidget->setItemsExpandable(true);
  m_treeWidget->setMinimumWidth(500);
  m_treeWidget->setSelectionMode(QAbstractItemView::SingleSelection);
  connect(m_treeWidget, &QTreeWidget::itemSelectionChanged, this, &ZChooseRegionDialog::updateSelectedID);
  m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

  lo->addWidget(m_label);
  lo->addWidget(m_treeWidget);
  lo->addWidget(m_buttonBox);

  m_treeWidget->setFocus();

  connect(m_buttonBox->button(QDialogButtonBox::Ok), &QPushButton::clicked,
          this, &ZChooseRegionDialog::accept);
  connect(m_buttonBox->button(QDialogButtonBox::Cancel), &QPushButton::clicked,
          this, &ZChooseRegionDialog::reject);
}

void ZChooseRegionDialog::updateSelectedID()
{
  QList<QTreeWidgetItem*> sis = m_treeWidget->selectedItems();
  if (sis.empty()) {
    m_selectedID = 0;
  } else {
    m_selectedID = sis[0]->data(0, Qt::UserRole).value<size_t>();
  }
}

} // namespace nim



