#include "zpunctatablemodel.h"

#include "zobjdoc.h"
#include "zlog.h"

namespace nim {

ZPunctaTableModel::ZPunctaTableModel(ZPunctaPack& p, QObject* parent)
  : QAbstractTableModel(parent)
  , m_puncta(p)
{}

QVariant ZPunctaTableModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid()) {
    return QVariant();
  }

  const auto& p = *m_puncta.punctaPts()[index.row()];

  if (role == Qt::DisplayRole) {
    switch (index.column()) {
      case ScoreColumn:
        return p.score();
      case XColumn:
        return p.x();
      case YColumn:
        return p.y();
      case ZColumn:
        return p.z();
      case RadiusColumn:
        return p.radius();
      case VolSizeColumn:
        return qulonglong(p.volSize());
      case MassColumn:
        return p.mass();
      case MeanIntensityColumn:
        return p.meanIntensity();
      case MaxIntensityColumn:
        return p.maxIntensity();
      case SDIntensityColumn:
        return p.sDevOfIntensity();
      default:
        break;
    }
  }

  return QVariant();
}

QVariant ZPunctaTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
    switch (section) {
      case ScoreColumn:
        return QString("Score");
      case XColumn:
        return QString("X");
      case YColumn:
        return QString("Y");
      case ZColumn:
        return QString("Z");
      case RadiusColumn:
        return QString("Radius");
      case VolSizeColumn:
        return QString("Volume Size");
      case MassColumn:
        return QString("Mass");
      case MeanIntensityColumn:
        return QString("Mean Intensity");
      case MaxIntensityColumn:
        return QString("Max Intensity");
      case SDIntensityColumn:
        return QString("Intensity SD");
      default:
        break;
    }
  }

  if (orientation == Qt::Vertical && role == Qt::DisplayRole) {
    return section;
  }

  return QVariant();
}

int ZPunctaTableModel::rowCount(const QModelIndex& parent) const
{
  if (parent.isValid()) {
    return 0;
  } else {
    return m_puncta.punctaPts().size();
  }
}

int ZPunctaTableModel::columnCount(const QModelIndex& /*parent*/) const
{
  return ColumnCount;
}

void ZPunctaTableModel::clicked(const QModelIndex& /*idxIn*/)
{
  //  if (idxIn.isValid()) {
  //    if (idxIn.column() == ViewSettingColumn) {
  //      ObjItem *item = static_cast<ObjItem*>(idxIn.internalPointer());
  //      if (m_viewSettingCurrentItem == item) {
  //        m_viewSettingCurrentItem = nullptr;
  //        m_regionAnnotation->sendHideViewSettingSignal();
  //      } else {
  //        QModelIndex prevIdx;
  //        for (int row = 0; row < rowCount(); ++row) {
  //          QModelIndex idx = index(row, ViewSettingColumn);
  //          if (static_cast<ObjItem*>(idx.internalPointer()) == m_viewSettingCurrentItem) {
  //            prevIdx = idx;
  //            break;
  //          }
  //          if (rowCount(idx) > 0) {
  //            for (int subRow = 0; subRow < rowCount(idx); ++subRow) {
  //              QModelIndex subIdx = index(subRow, ViewSettingColumn, idx);
  //              if (static_cast<ObjItem*>(subIdx.internalPointer()) == m_viewSettingCurrentItem) {
  //                prevIdx = subIdx;
  //                break;
  //              }
  //            }
  //          }
  //          if (prevIdx.isValid())
  //            break;
  //        }

  //        m_viewSettingCurrentItem = item;
  //        if (prevIdx.isValid())
  //          Q_EMIT dataChanged(prevIdx, prevIdx);
  //        m_regionAnnotation->sendShowViewSettingSignal(item->id);
  //      }
  //      Q_EMIT dataChanged(idxIn, idxIn);
  //    }
  //  }
}

void ZPunctaTableModel::doubleClicked(const QModelIndex&) {}

void ZPunctaTableModel::activated(const QModelIndex&)
{
  //  size_t id = indexToId(idxIn);
  //  if (id > 0) {
  //    //VLOG(1) << id;
  //    m_regionAnnotation->sendOpenEditWidgetSignal(id);
  //  }
}

void ZPunctaTableModel::updateModel()
{
  beginResetModel();
  endResetModel();
}

} // namespace nim
