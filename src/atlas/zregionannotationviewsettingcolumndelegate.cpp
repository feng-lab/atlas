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

    m_currentID = m_idToROIFilters.begin()->first;
    std::shared_ptr<ZWidgetsGroup> wgg = m_idToROIFilters[m_currentID]->viewSettingWidgetsGroupForAnnotationFilter();
    auto wgi = wgg->createWidget(true, false);
    wgi->setParent(m_widget);
    m_actualWidget.reset(wgi);

    m_isOneCellInEditMode = false;
  }
}

QWidget*
ZRegionAnnotationViewSettingColumnDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                                                         const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    auto region = index.data(Qt::UserRole).toLongLong();
    LOG(INFO) << region;
    std::shared_ptr<ZWidgetsGroup> wg = m_idToROIFilters[region]->viewSettingWidgetsGroupForAnnotationFilter();
    LOG(INFO) << "here";
    auto wgi = wg->createWidget(true, false);
    LOG(INFO) << "here";
    wgi->setParent(parent);
    m_actualWidget.reset(wgi);
    return wgi;
  } else {
    return QStyledItemDelegate::createEditor(parent, option, index);
  }
}

void ZRegionAnnotationViewSettingColumnDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    if (auto wgt = qobject_cast<QWidget*>(editor)) {
      wgt->setProperty("user_data", index.data(Qt::UserRole));
    }
    //LOG(INFO) << "set " << btn->property("user_data");
  } else {
    QStyledItemDelegate::setEditorData(editor, index);
  }
}

void ZRegionAnnotationViewSettingColumnDelegate::setModelData(QWidget* editor, QAbstractItemModel* model,
                                                              const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    if (auto wgt = qobject_cast<QWidget*>(editor)) {
      model->setData(index, wgt->property("user_data"), Qt::UserRole);
    }
    //LOG(INFO) << btn->property("user_data");
  } else {
    QStyledItemDelegate::setModelData(editor, model, index);
  }
}

void ZRegionAnnotationViewSettingColumnDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                                       const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    if (m_actualWidget) {
      QRect rect = option.rect;
      m_actualWidget->setGeometry(rect);
      if (option.state == QStyle::State_Selected)
        painter->fillRect(rect, option.palette.highlight());
      QPixmap map = m_actualWidget->grab();
      painter->drawPixmap(option.rect, map);
    }
  } else {
    QStyledItemDelegate::paint(painter, option, index);
  }
}

void
ZRegionAnnotationViewSettingColumnDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                                                                 const QModelIndex& /*index*/) const
{
  editor->setGeometry(option.rect);
}

QSize
ZRegionAnnotationViewSettingColumnDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    if (m_actualWidget) {
      QSize res = m_actualWidget->grab().size();

      res.setWidth(res.width());
      res.setHeight(res.height() / qApp->devicePixelRatio());
      return res;
    }
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

} // namespace nim
