#include "zregionannotationviewsettingcolumndelegate.h"

#include "zlog.h"
#include "zclickablelabel.h"
#include "zroifilter.h"
#include "z3dmeshfilter.h"
#include <QPainter>
#include <QAbstractItemView>

namespace nim {

ZRegionAnnotationViewSettingColumnDelegate::ZRegionAnnotationViewSettingColumnDelegate(
  std::map<int64_t, std::unique_ptr<ZROIFilter>>& idToROIFilters,
  QObject* parent)
  : ZRegionAnnotationViewSettingColumnDelegate(parent)
{
  m_idToROIFilters = &idToROIFilters;
}

ZRegionAnnotationViewSettingColumnDelegate::ZRegionAnnotationViewSettingColumnDelegate(
  std::map<int64_t, std::unique_ptr<Z3DMeshFilter>>& idToMeshFilters,
  QObject* parent)
  : ZRegionAnnotationViewSettingColumnDelegate(parent)
{
  m_idToMeshFilters = &idToMeshFilters;
}

void ZRegionAnnotationViewSettingColumnDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.isValid() && index.model()->headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    bool ok;
    int64_t regionID = index.data(Qt::UserRole).toLongLong(&ok);
    CHECK(ok);
    //LOG(INFO) << "painting " << regionID;
    QRect rect = option.rect;
    QWidget* wgt = nullptr;
    if (m_idToROIFilters) {
      wgt = new ZRegionViewSettingLabel(m_idToROIFilters->at(regionID).get());
    } else if (m_idToMeshFilters) {
      wgt = new Z3DRegionViewSettingLabel(m_idToMeshFilters->at(regionID).get());
    } else {
      CHECK(false);
    }
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
    if (m_idToROIFilters) {
      return ZRegionViewSettingLabel::staticMinimumSizeHint();
    } else if (m_idToMeshFilters) {
      return Z3DRegionViewSettingLabel::staticMinimumSizeHint();
    } else {
      CHECK(false);
    }
  }
  return QStyledItemDelegate::sizeHint(option, index);
}

ZRegionAnnotationViewSettingColumnDelegate::ZRegionAnnotationViewSettingColumnDelegate(QObject* parent)
  : QStyledItemDelegate(parent)
{
}

} // namespace nim
