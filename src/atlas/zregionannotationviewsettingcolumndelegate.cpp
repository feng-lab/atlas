#include "zregionannotationviewsettingcolumndelegate.h"

#include "zlog.h"
#include "zregionviewsettingwitheditorwindow.h"
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

#ifdef USE_BUTTON
    m_button = new QPushButton("...", m_widget);
    m_button->hide();
#endif

    m_isOneCellInEditMode = false;
  }
}

QWidget*
ZRegionAnnotationViewSettingColumnDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
#ifdef USE_BUTTON
    auto btn = new QPushButton(parent);
    btn->setText("...");
    connect(btn, &QPushButton::clicked, this, &ZRegionAnnotationViewSettingColumnDelegate::buttonClicked);
    return btn;
#else
    bool ok;
    int64_t regionID = index.data(Qt::UserRole).toLongLong(&ok);
    CHECK(ok);
    auto wgt = new ZRegionViewSettingWithEditorWindow(m_idToROIFilters.at(regionID).get(), parent);
    return wgt;
#endif
  } else {
    return QStyledItemDelegate::createEditor(parent, option, index);
  }
}

#ifdef USE_BUTTON
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
#endif

void ZRegionAnnotationViewSettingColumnDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
#ifdef USE_BUTTON
    QRect rect = option.rect;
    m_button->setGeometry(rect);
    m_button->setText("...");
    // m_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    if (option.state == QStyle::State_Selected)
      painter->fillRect(rect, option.palette.highlight());
    QPixmap map = m_button->grab();
    painter->drawPixmap(option.rect, map);
#else
    bool ok;
    int64_t regionID = index.data(Qt::UserRole).toLongLong(&ok);
    CHECK(ok);
    QRect rect = option.rect;
    auto wgt = new ZRegionViewSettingWithEditorWindow(m_idToROIFilters.at(regionID).get(), m_widget);
    wgt->setGeometry(rect);
    if (option.state == QStyle::State_Selected)
      painter->fillRect(rect, option.palette.highlight());
    QPixmap map = wgt->grab();
    delete wgt;
    painter->drawPixmap(option.rect, map);
#endif
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
#ifdef USE_BUTTON
    return QSize(32, 32);
#else
    return QSize(50, 50);
#endif
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

#ifdef USE_BUTTON
void ZRegionAnnotationViewSettingColumnDelegate::buttonClicked()
{
  if (QPushButton* btn = qobject_cast<QPushButton*>(sender())) {
    emit buttonClickedForUserData(btn->property("user_data"));
  }
}
#endif

} // namespace nim
