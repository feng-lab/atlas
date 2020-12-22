#include "zsaveobjsdialog.h"

#include "zobjdoc.h"
#include "zdoc.h"
#include <QPushButton>
#include <QLabel>
#include <QDialogButtonBox>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QDir>

namespace nim {

ZSaveObjsDialog::ZSaveObjsDialog(const ZDoc& doc, const std::vector<size_t>& objs, QWidget* parent)
  : QDialog(parent), m_doc(doc)
{
  createWidget();

  for (auto id : objs) {
    ZObjDoc* odoc = m_doc.idToDoc(id);
    QTreeWidgetItem* item = new QTreeWidgetItem(m_treeWidget, QStringList()
      << odoc->objName(id) << QDir::toNativeSeparators(odoc->objPath(id)));
    item->setData(0, Qt::UserRole, QVariant::fromValue(id));
  }

  m_treeWidget->resizeColumnToContents(0);
  m_treeWidget->selectAll();
#ifdef __APPLE__
  m_treeWidget->setAlternatingRowColors(true);
#endif
  adjustButtonWidths();
  updateSaveButton();

  connect(m_treeWidget, &QTreeWidget::itemSelectionChanged, this, &ZSaveObjsDialog::updateSaveButton);
}

void ZSaveObjsDialog::createWidget()
{
  QVBoxLayout* lo = new QVBoxLayout(this);
  m_label = new QLabel("The following objects have unsaved changes:", this);
  m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_treeWidget = new QTreeWidget(this);
  m_treeWidget->setColumnCount(2);
  m_treeWidget->setHeaderHidden(true);
  m_treeWidget->setItemsExpandable(true);
  m_treeWidget->setMinimumWidth(500);
  m_treeWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
#ifdef __APPLE__
  QDialogButtonBox::ButtonRole discardButtonRole = QDialogButtonBox::ResetRole;
#else
  QDialogButtonBox::ButtonRole discardButtonRole = QDialogButtonBox::DestructiveRole;
#endif
  QPushButton* discardButton = m_buttonBox->addButton(tr("Do not Save"), discardButtonRole);

  lo->addWidget(m_label);
  lo->addWidget(m_treeWidget);
  lo->addWidget(m_buttonBox);

  m_treeWidget->setFocus();

  connect(m_buttonBox->button(QDialogButtonBox::Save), &QPushButton::clicked,
          this, &ZSaveObjsDialog::collectObjsToSave);
  connect(m_buttonBox->button(QDialogButtonBox::Cancel), &QPushButton::clicked,
          this, &ZSaveObjsDialog::reject);
  connect(discardButton, &QPushButton::clicked, this, &ZSaveObjsDialog::discard);
}

void ZSaveObjsDialog::collectObjsToSave()
{
  m_objsToSave.clear();
  for (auto item : m_treeWidget->selectedItems()) {
    m_objsToSave.push_back(item->data(0, Qt::UserRole).value<size_t>());
  }
  accept();
}

void ZSaveObjsDialog::discard()
{
  m_treeWidget->clearSelection();
  collectObjsToSave();
}

void ZSaveObjsDialog::updateSaveButton()
{
  int count = m_treeWidget->selectedItems().count();
  QPushButton* button = m_buttonBox->button(QDialogButtonBox::Save);
  if (count == m_treeWidget->topLevelItemCount()) {
    button->setEnabled(true);
    button->setText(tr("Save All"));
  } else if (count == 0) {
    button->setEnabled(false);
    button->setText(tr("Save"));
  } else {
    button->setEnabled(true);
    button->setText(tr("Save Selected"));
  }
}

void ZSaveObjsDialog::adjustButtonWidths()
{
  QStringList possibleTexts;
  possibleTexts << tr("Save") << tr("Save All");
  if (m_treeWidget->topLevelItemCount() > 1)
    possibleTexts << tr("Save Selected");
  int maxTextWidth = 0;
  QPushButton* saveButton = m_buttonBox->button(QDialogButtonBox::Save);
  for (const QString& text : possibleTexts) {
    saveButton->setText(text);
    int hint = saveButton->sizeHint().width();
    if (hint > maxTextWidth)
      maxTextWidth = hint;
  }
  saveButton->setMinimumWidth(maxTextWidth);
}

} // namespace nim
