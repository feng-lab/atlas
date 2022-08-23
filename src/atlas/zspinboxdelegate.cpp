#include "zspinboxdelegate.h"

#include <QSpinBox>

namespace nim {

ZSpinBoxDelegate::ZSpinBoxDelegate(QObject* parent)
  : QStyledItemDelegate(parent)
{}

QWidget* ZSpinBoxDelegate::createEditor(QWidget* parent,
                                        const QStyleOptionViewItem& /* option */,
                                        const QModelIndex& /* index */) const
{
  auto* editor = new QSpinBox(parent);
  editor->setFrame(false);
  editor->setMinimum(0);
  editor->setMaximum(100);

  return editor;
}

void ZSpinBoxDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
{
  int value = index.model()->data(index, Qt::EditRole).toInt();

  auto* spinBox = static_cast<QSpinBox*>(editor);
  spinBox->setValue(value);
}

void ZSpinBoxDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const
{
  auto* spinBox = static_cast<QSpinBox*>(editor);
  spinBox->interpretText();
  int value = spinBox->value();

  model->setData(index, value, Qt::EditRole);
}

void ZSpinBoxDelegate::updateEditorGeometry(QWidget* editor,
                                            const QStyleOptionViewItem& option,
                                            const QModelIndex& /* index */) const
{
  editor->setGeometry(option.rect);
}

} // namespace nim
