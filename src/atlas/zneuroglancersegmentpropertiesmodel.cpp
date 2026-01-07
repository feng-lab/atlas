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

  const ZNeuroglancerPrecomputedSegmentProperties::Property* labelProp = nullptr;
  const ZNeuroglancerPrecomputedSegmentProperties::Property* descriptionProp = nullptr;
  const ZNeuroglancerPrecomputedSegmentProperties::Property* tagsProp = nullptr;
  for (const auto& p : m_props->properties()) {
    switch (p.type) {
    case ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Label:
      labelProp = &p;
      break;
    case ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Description:
      descriptionProp = &p;
      break;
    case ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Tags:
      tagsProp = &p;
      break;
    default:
      break;
    }
  }

  // Column order: ID, then the semantic "primary" fields, then the remaining properties in file order.
  m_columns.clear();
  {
    ColumnSpec id;
    id.kind = ColumnSpec::Kind::Id;
    id.header = QStringLiteral("ID");
    m_columns.push_back(std::move(id));
  }

  auto addPropColumn = [this](const ZNeuroglancerPrecomputedSegmentProperties::Property& p, QString headerOverride) {
    ColumnSpec c;
    c.prop = &p;
    c.header = headerOverride.trimmed().isEmpty() ? p.id : std::move(headerOverride);
    switch (p.type) {
    case ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Label:
    case ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Description:
    case ZNeuroglancerPrecomputedSegmentProperties::PropertyType::String:
      c.kind = ColumnSpec::Kind::String;
      break;
    case ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Number:
      c.kind = ColumnSpec::Kind::Number;
      break;
    case ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Tags:
      c.kind = ColumnSpec::Kind::Tags;
      break;
    }
    m_columns.push_back(std::move(c));
  };

  if (labelProp) {
    addPropColumn(*labelProp, QStringLiteral("Label"));
  }
  if (descriptionProp) {
    addPropColumn(*descriptionProp, QStringLiteral("Description"));
  }
  if (tagsProp) {
    addPropColumn(*tagsProp, QStringLiteral("Tags"));
  }

  for (const auto& p : m_props->properties()) {
    if (&p == labelProp || &p == descriptionProp || &p == tagsProp) {
      continue;
    }
    addPropColumn(p, {});
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
  return static_cast<int>(m_columns.size());
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

  const uint64_t id = m_props->ids().at(static_cast<size_t>(row));

  if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
    const ColumnSpec& c = m_columns.at(static_cast<size_t>(col));
    if (c.kind == ColumnSpec::Kind::Id) {
      return QString::number(static_cast<qulonglong>(id));
    }
    CHECK(c.prop);

    switch (c.kind) {
    case ColumnSpec::Kind::Id:
      CHECK(false);
      return {};
    case ColumnSpec::Kind::String:
      CHECK(c.prop->stringValues.size() == m_props->numIds());
      return c.prop->stringValues.at(static_cast<size_t>(row));
    case ColumnSpec::Kind::Number:
      CHECK(c.prop->numberValues.size() == m_props->numIds());
      return c.prop->numberValues.at(static_cast<size_t>(row));
    case ColumnSpec::Kind::Tags: {
      CHECK(c.prop->tagIndicesValues.size() == m_props->numIds());
      const auto& indices = c.prop->tagIndicesValues.at(static_cast<size_t>(row));
      QStringList tags;
      tags.reserve(static_cast<int>(indices.size()));
      for (const uint32_t idx : indices) {
        CHECK(idx < c.prop->tags.size());
        tags << c.prop->tags.at(static_cast<size_t>(idx));
      }
      return tags.join(", ");
    }
    }
  }

  return {};
}

QVariant ZNeuroglancerSegmentPropertiesModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (orientation == Qt::Horizontal) {
    if (section < 0 || section >= columnCount()) {
      return {};
    }
    const ColumnSpec& c = m_columns.at(static_cast<size_t>(section));
    if (role == Qt::DisplayRole) {
      return c.header;
    }
    if (role == Qt::ToolTipRole && c.prop) {
      if (!c.prop->description.trimmed().isEmpty()) {
        return c.prop->description.trimmed();
      }
      if (c.prop->type == ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Tags) {
        return QStringLiteral("Tag list (%1 values)").arg(static_cast<int>(c.prop->tags.size()));
      }
    }
    return {};
  }
  if (role != Qt::DisplayRole) {
    return {};
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

} // namespace nim
