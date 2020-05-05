#include "z3dregionannotationviewsettingcolumndelegate.h"

#include "zlog.h"
#include "zclickablelabel.h"
#include <QPainter>
#include <QStylePainter>
#include <QAbstractItemView>
#include <QApplication>

namespace nim {

Z3DRegionAnnotationViewSettingColumnDelegate::Z3DRegionAnnotationViewSettingColumnDelegate(
  std::map<int, std::unique_ptr<Z3DMeshFilter>>& idToMeshFilters,
  QObject* parent)
  : QStyledItemDelegate(parent)
  , m_idToMeshFilters(idToMeshFilters)
{
}

void Z3DRegionAnnotationViewSettingColumnDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    bool ok;
    int64_t regionID = index.data(Qt::UserRole).toLongLong(&ok);
    CHECK(ok);
    //LOG(INFO) << "painting " << regionID;
    QRect rect = option.rect;
    auto wgt = new Z3DRegionViewSettingLabel(m_idToMeshFilters.at(regionID).get());
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

QSize Z3DRegionAnnotationViewSettingColumnDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    return QSize(40, 40);
  }
  return QStyledItemDelegate::sizeHint(option, index);
}

} // namespace nim
