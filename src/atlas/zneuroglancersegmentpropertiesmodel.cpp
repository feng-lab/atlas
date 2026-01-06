#include "zneuroglancersegmentpropertiesmodel.h"

#include "zlog.h"

#include <QStringList>

namespace nim {

ZNeuroglancerSegmentPropertiesModel::ZNeuroglancerSegmentPropertiesModel(
  std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> props,
  QObject* parent)
  : QAbstractTableModel(parent)
  , m_props(std::move(props))
{
  CHECK(m_props);

  for (const auto& p : m_props->properties()) {
    switch (p.type) {
    case ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Label:
      m_labelProp = &p;
      break;
    case ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Description:
      m_descriptionProp = &p;
      break;
    case ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Tags:
      m_tagsProp = &p;
      break;
    default:
      break;
    }
  }
}

int ZNeuroglancerSegmentPropertiesModel::rowCount(const QModelIndex& parent) const
{
  if (parent.isValid()) {
    return 0;
  }
  CHECK(m_props);
  return static_cast<int>(m_props->numIds());
}

int ZNeuroglancerSegmentPropertiesModel::columnCount(const QModelIndex& parent) const
{
  if (parent.isValid()) {
    return 0;
  }
  return static_cast<int>(Column::Count);
}

QVariant ZNeuroglancerSegmentPropertiesModel::data(const QModelIndex& index, int role) const
{
  CHECK(m_props);
  if (!index.isValid()) {
    return {};
  }
  const int row = index.row();
  const int col = index.column();
  if (row < 0 || row >= rowCount() || col < 0 || col >= columnCount()) {
    return {};
  }

  const Column column = static_cast<Column>(col);
  const uint64_t id = m_props->ids().at(static_cast<size_t>(row));

  if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
    switch (column) {
    case Column::Id:
      return QString::number(static_cast<qulonglong>(id));
    case Column::Label:
      if (!m_labelProp) {
        return {};
      }
      CHECK(m_labelProp->stringValues.size() == m_props->numIds());
      return m_labelProp->stringValues.at(static_cast<size_t>(row));
    case Column::Description:
      if (!m_descriptionProp) {
        return {};
      }
      CHECK(m_descriptionProp->stringValues.size() == m_props->numIds());
      return m_descriptionProp->stringValues.at(static_cast<size_t>(row));
    case Column::Tags:
      if (!m_tagsProp) {
        return {};
      }
      CHECK(m_tagsProp->tagIndicesValues.size() == m_props->numIds());
      {
        const auto& indices = m_tagsProp->tagIndicesValues.at(static_cast<size_t>(row));
        QStringList tags;
        tags.reserve(static_cast<int>(indices.size()));
        for (const uint32_t idx : indices) {
          CHECK(idx < m_tagsProp->tags.size());
          tags << m_tagsProp->tags.at(static_cast<size_t>(idx));
        }
        return tags.join(", ");
      }
    case Column::Count:
      break;
    }
  }

  return {};
}

QVariant ZNeuroglancerSegmentPropertiesModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (role != Qt::DisplayRole) {
    return {};
  }
  if (orientation == Qt::Horizontal) {
    if (section < 0 || section >= static_cast<int>(Column::Count)) {
      return {};
    }
    return columnName(static_cast<Column>(section));
  }
  return section + 1;
}

std::optional<uint64_t> ZNeuroglancerSegmentPropertiesModel::segmentIdForRow(int row) const
{
  if (!m_props) {
    return std::nullopt;
  }
  if (row < 0 || row >= rowCount()) {
    return std::nullopt;
  }
  return m_props->ids().at(static_cast<size_t>(row));
}

QString ZNeuroglancerSegmentPropertiesModel::columnName(Column c)
{
  switch (c) {
  case Column::Id:
    return QStringLiteral("ID");
  case Column::Label:
    return QStringLiteral("Label");
  case Column::Description:
    return QStringLiteral("Description");
  case Column::Tags:
    return QStringLiteral("Tags");
  case Column::Count:
    break;
  }
  return {};
}

} // namespace nim
