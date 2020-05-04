#include "zregionannotationviewsettingcolumndelegate.h"

#include "zlog.h"
#include <QPainter>
#include <QPushButton>
#include <QStylePainter>
#include <QAbstractItemView>
#include <QApplication>

namespace nim {

ZRegionAnnotationViewSettingColumnDelegate::ZRegionAnnotationViewSettingColumnDelegate(
  std::map<int, std::unique_ptr<ZROIFilter>>& idToROIFilters,
  QObject* parent)
  : QStyledItemDelegate(parent)
  , m_idToROIFilters(idToROIFilters)
{
  if (auto wg = qobject_cast<QAbstractItemView*>(parent)) {
    m_widget = wg;

    m_button = new QPushButton("...", m_widget);
    m_button->hide();

    m_isOneCellInEditMode = false;
  }
}

QWidget*
ZRegionAnnotationViewSettingColumnDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    auto btn = new QPushButton(parent);
    btn->setText("...");
    connect(btn, &QPushButton::clicked, this, &ZRegionAnnotationViewSettingColumnDelegate::buttonClicked);
    return btn;
  } else {
    return QStyledItemDelegate::createEditor(parent, option, index);
  }
}

void ZRegionAnnotationViewSettingColumnDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
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

void ZRegionAnnotationViewSettingColumnDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const
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

void ZRegionAnnotationViewSettingColumnDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    // LOG(INFO) << "here";
    QRect rect = option.rect;
    m_button->setGeometry(rect);
    m_button->setText("...");
    // m_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    if (option.state == QStyle::State_Selected)
      painter->fillRect(rect, option.palette.highlight());
    QPixmap map = m_button->grab();
    painter->drawPixmap(option.rect, map);
  } else {
    QStyledItemDelegate::paint(painter, option, index);
  }
}

void ZRegionAnnotationViewSettingColumnDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                                                 const QModelIndex& /*index*/) const
{
  editor->setGeometry(option.rect);
}

QSize ZRegionAnnotationViewSettingColumnDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    return QSize(32, 32);
  }
  return QStyledItemDelegate::sizeHint(option, index);
}

void ZRegionAnnotationViewSettingColumnDelegate::cellEntered(const QModelIndex& index)
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

void ZRegionAnnotationViewSettingColumnDelegate::buttonClicked()
{
  if (QPushButton* btn = qobject_cast<QPushButton*>(sender())) {
    emit buttonClickedForUserData(btn->property("user_data"));
  }
}

} // namespace nim
