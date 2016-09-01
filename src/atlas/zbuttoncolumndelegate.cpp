#include "zbuttoncolumndelegate.h"

#include "zlog.h"
#include <QPainter>
#include <QPushButton>
#include <QStylePainter>
#include <QAbstractItemView>

namespace nim {

ZButtonColumnDelegate::ZButtonColumnDelegate(QObject* parent)
  : QStyledItemDelegate(parent)
  , m_button(nullptr)
{
  if (QAbstractItemView* wg = qobject_cast<QAbstractItemView*>(parent)) {
    m_widget = wg;
    m_button = new QPushButton("...", m_widget);
    m_button->hide();
    m_isOneCellInEditMode = false;
  }
}

ZButtonColumnDelegate::~ZButtonColumnDelegate()
{

}

QWidget*
ZButtonColumnDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    auto btn = new QPushButton(parent);
    btn->setText(index.data().toString());
    connect(btn, &QPushButton::clicked, this, &ZButtonColumnDelegate::buttonClicked);
    return btn;
  } else {
    return QStyledItemDelegate::createEditor(parent, option, index);
  }
}

void ZButtonColumnDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    if (QPushButton* btn = qobject_cast<QPushButton*>(editor)) {
      btn->setProperty("user_data", index.data(Qt::UserRole));
    }
    //LOG(INFO) << "set " << btn->property("user_data");
  } else {
    QStyledItemDelegate::setEditorData(editor, index);
  }
}

void ZButtonColumnDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    if (QPushButton* btn = qobject_cast<QPushButton*>(editor)) {
      model->setData(index, btn->property("user_data"), Qt::UserRole);
    }
    //LOG(INFO) << btn->property("user_data");
  } else {
    QStyledItemDelegate::setModelData(editor, model, index);
  }
}

void ZButtonColumnDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    QRect rect = option.rect;
    m_button->setGeometry(rect);
    m_button->setText(index.data().toString());
    //m_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    if (option.state == QStyle::State_Selected)
      painter->fillRect(rect, option.palette.highlight());
    QPixmap map = m_button->grab();
    painter->drawPixmap(option.rect, map);
  } else {
    QStyledItemDelegate::paint(painter, option, index);
  }
}

void ZButtonColumnDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                                                 const QModelIndex& /*index*/) const
{
  editor->setGeometry(option.rect);
}

QSize ZButtonColumnDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    m_button->setText(index.data().toString());
    QSize res = m_button->grab().size();
    res.setWidth(res.width() * 2);
    return res;
  }
  return QStyledItemDelegate::sizeHint(option, index);
}

void ZButtonColumnDelegate::cellEntered(const QModelIndex& index)
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole) == 1) {
    if (m_isOneCellInEditMode) {
      m_widget->closePersistentEditor(m_currentEditedCellIndex);
    }
    m_widget->openPersistentEditor(index);
    m_isOneCellInEditMode = true;
    m_currentEditedCellIndex = index;
  } else {
    if (m_isOneCellInEditMode) {
      m_isOneCellInEditMode = false;
      m_widget->closePersistentEditor(m_currentEditedCellIndex);
    }
  }
}

void ZButtonColumnDelegate::buttonClicked()
{
  if (QPushButton* btn = qobject_cast<QPushButton*>(sender())) {
    emit buttonClickedForUserData(btn->property("user_data"));
  }
}

} // namespace nim
