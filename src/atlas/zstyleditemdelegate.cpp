#include "zstyleditemdelegate.h"

#if 0
#include <QEvent>
#include <QApplication>
#include <QPushButton>
#include <QPainter>
#include <QAbstractItemView>
#include "zlog.h"
#endif

namespace nim {

ZStyledItemDelegate::ZStyledItemDelegate(QObject *parent)
  : QStyledItemDelegate(parent)
  //, m_settingIcon(":/icons/settings-512.png")
{
  setItemEditorFactory(&m_factory);

#if 0
  m_widget = qobject_cast<QAbstractItemView*>(parent);
  m_btn = new QPushButton();
  m_btn->setIcon(m_settingIcon);
  m_btn->setCheckable(true);
  m_btn->hide();
#endif
}

#if 0
void ZStyledItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
  if(index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    m_btn->setGeometry(option.rect);

    if (option.state == QStyle::State_Selected)
      painter->fillRect(option.rect, option.palette.highlight());
    QPixmap map = m_btn->grab();
    painter->drawPixmap(option.rect.x(),option.rect.y(),map);
  } else {
    QStyledItemDelegate::paint(painter,option, index);
  }
}

QWidget *ZStyledItemDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
  if (index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    QPushButton * btn = new QPushButton(parent);
    btn->setIcon(m_settingIcon);
    btn->setCheckable(true);
    btn->setAutoExclusive(true);
    return btn;
  } else {
    return QStyledItemDelegate::createEditor(parent, option, index);
  }
}

void ZStyledItemDelegate::setEditorData(QWidget *editor, const QModelIndex &index) const
{
  if (index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    QPushButton * btn = qobject_cast<QPushButton *>(editor);
    btn->setProperty("data_value", index.data());
    LINFO() << index.data() << " 1";
  } else {
    QStyledItemDelegate::setEditorData(editor, index);
  }
}

void ZStyledItemDelegate::setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const
{
  if (index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    QPushButton *btn = qobject_cast<QPushButton *>(editor);
    model->setData(index, btn->property("data_value"));
    LINFO() << index.data() << " 2";
  } else {
    QStyledItemDelegate::setModelData(editor, model, index);
  }
}

void ZStyledItemDelegate::updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &) const
{
  editor->setGeometry(option.rect);
}

void ZStyledItemDelegate::cellEntered(const QModelIndex &index)
{
  if(index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole) == 1) {
    if(m_isOneCellInEditMode) {
      m_widget->closePersistentEditor(m_currentEditedCellIndex);
    }
    m_widget->openPersistentEditor(index);
    m_isOneCellInEditMode = true;
    m_currentEditedCellIndex = index;
  } else {
    if(m_isOneCellInEditMode) {
      m_isOneCellInEditMode = false;
      m_widget->closePersistentEditor(m_currentEditedCellIndex);
    }
  }
}
#endif

} // namespace nim
