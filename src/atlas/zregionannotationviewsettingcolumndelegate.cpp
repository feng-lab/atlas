#include "zregionannotationviewsettingcolumndelegate.h"

#include "zlog.h"
#include "zclickablelabel.h"
#include <QPainter>
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
}

void ZRegionAnnotationViewSettingColumnDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    bool ok;
    int64_t regionID = index.data(Qt::UserRole).toLongLong(&ok);
    CHECK(ok);
    //LOG(INFO) << "painting " << regionID;
    QRect rect = option.rect;
    auto wgt = new ZRegionViewSettingLabel(m_idToROIFilters.at(regionID).get());
    wgt->setGeometry(rect);
//    if (option.state == QStyle::State_Selected)
//      painter->fillRect(rect, option.palette.highlight());
    QPixmap map = wgt->grab();
    delete wgt;
    painter->drawPixmap(option.rect, map);
  } else {
    QStyledItemDelegate::paint(painter, option, index);
  }
}

QSize ZRegionAnnotationViewSettingColumnDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    return QSize(50, 50);
  }
  return QStyledItemDelegate::sizeHint(option, index);
}

} // namespace nim
