#include "zneuroglancerprecomputedsegmentproperties.h"

#include "zexception.h"
#include "zjson.h"
#include "zproxygenhttpclient.h"
#include "zlog.h"

#include <folly/coro/BlockingWait.h>

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace nim {
namespace json = boost::json;

namespace {

std::string toStdString(const QString& s)
{
  const auto u8 = s.toUtf8();
  return std::string(u8.data(), static_cast<size_t>(u8.size()));
}

QString requireString(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end() || !it->value().is_string()) {
    throw ZException(fmt::format("Missing or invalid '{}' in neuroglancer segment properties info", key));
  }
  return json::value_to<QString>(it->value());
}

std::optional<QString> optionalString(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end() || it->value().is_null()) {
    return std::nullopt;
  }
  if (!it->value().is_string()) {
    throw ZException(fmt::format("Invalid '{}' in neuroglancer segment properties info (expected string)", key));
  }
  return json::value_to<QString>(it->value());
}

ZNeuroglancerPrecomputedSegmentProperties::PropertyType parsePropertyType(QString s)
{
  s = s.trimmed().toLower();
  if (s == "label") {
    return ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Label;
  }
  if (s == "description") {
    return ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Description;
  }
  if (s == "string") {
    return ZNeuroglancerPrecomputedSegmentProperties::PropertyType::String;
  }
  if (s == "tags") {
    return ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Tags;
  }
  if (s == "number") {
    return ZNeuroglancerPrecomputedSegmentProperties::PropertyType::Number;
  }
  throw ZException(fmt::format("Unsupported segment property type '{}'", toStdString(s)));
}

ZNeuroglancerPrecomputedSegmentProperties::NumberDataType parseNumberDataType(QString s)
{
  s = s.trimmed().toLower();
  if (s == "uint8") {
    return ZNeuroglancerPrecomputedSegmentProperties::NumberDataType::Uint8;
  }
  if (s == "int8") {
    return ZNeuroglancerPrecomputedSegmentProperties::NumberDataType::Int8;
  }
  if (s == "uint16") {
    return ZNeuroglancerPrecomputedSegmentProperties::NumberDataType::Uint16;
  }
  if (s == "int16") {
    return ZNeuroglancerPrecomputedSegmentProperties::NumberDataType::Int16;
  }
  if (s == "uint32") {
    return ZNeuroglancerPrecomputedSegmentProperties::NumberDataType::Uint32;
  }
  if (s == "int32") {
    return ZNeuroglancerPrecomputedSegmentProperties::NumberDataType::Int32;
  }
  if (s == "float32") {
    return ZNeuroglancerPrecomputedSegmentProperties::NumberDataType::Float32;
  }
  throw ZException(fmt::format("Unsupported segment property number data_type '{}'", toStdString(s)));
}

uint64_t parseUint64Base10(QString s)
{
  s = s.trimmed();
  if (s.isEmpty()) {
    throw ZException("Invalid segment_properties id: empty string");
  }
  bool ok = false;
  const qulonglong v = s.toULongLong(&ok, 10);
  if (!ok) {
    throw ZException(fmt::format("Invalid segment_properties id '{}': expected base-10 uint64 string", toStdString(s)));
  }
  return static_cast<uint64_t>(v);
}

std::vector<QString> requireStringArray(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end() || !it->value().is_array()) {
    throw ZException(fmt::format("Missing or invalid '{}' (expected array) in neuroglancer segment properties info", key));
  }
  const auto& arr = it->value().as_array();
  std::vector<QString> out;
  out.reserve(arr.size());
  for (const auto& v : arr) {
    if (!v.is_string()) {
      throw ZException(fmt::format("Invalid '{}' in neuroglancer segment properties info (expected string elements)", key));
    }
    out.push_back(json::value_to<QString>(v));
  }
  return out;
}

std::string joinKeys(const json::object& obj)
{
  std::string out;
  bool first = true;
  for (const auto& kv : obj) {
    if (!first) {
      out += ", ";
    }
    first = false;
    out += kv.key_c_str();
  }
  return out;
}

} // namespace

std::shared_ptr<ZNeuroglancerPrecomputedSegmentProperties> ZNeuroglancerPrecomputedSegmentProperties::open(
  const QUrl& dirUrl,
  std::chrono::milliseconds timeout)
{
  QUrl infoUrl = dirUrl.resolved(QUrl("info"));
  const std::string infoUrlStr = toStdString(infoUrl.toString());

  auto resOpt = folly::coro::blockingWait(ZProxygenHttpClient::instance().getBytes(infoUrlStr, timeout));
  if (!resOpt) {
    throw ZException(fmt::format("Segment properties info not found (HTTP 404) at '{}'", infoUrlStr));
  }
  if (resOpt->status != 200) {
    throw ZException(fmt::format("Failed to fetch segment properties info from '{}' (HTTP {})", infoUrlStr, resOpt->status));
  }

  const std::string infoText(reinterpret_cast<const char*>(resOpt->body.data()), resOpt->body.size());
  return parseInfoJsonText(dirUrl, infoText);
}

std::shared_ptr<ZNeuroglancerPrecomputedSegmentProperties> ZNeuroglancerPrecomputedSegmentProperties::parseInfoJsonText(
  const QUrl& dirUrl,
  std::string_view infoJsonText)
{
  auto props = std::shared_ptr<ZNeuroglancerPrecomputedSegmentProperties>(new ZNeuroglancerPrecomputedSegmentProperties());
  props->m_dirUrl = dirUrl;

  json::value jv = json::parse(infoJsonText);
  if (!jv.is_object()) {
    throw ZException("Segment properties info is not a JSON object");
  }
  const auto& root = jv.as_object();

  const QString type = requireString(root, "@type").trimmed();
  if (type != "neuroglancer_segment_properties") {
    throw ZException(fmt::format("Unsupported segment properties '@type': '{}'", toStdString(type)));
  }

  auto parseInlineObject = [&props](const json::object& inl) {
    // ids
    auto idsIt = inl.find("ids");
    if (idsIt == inl.end() || !idsIt->value().is_array()) {
      throw ZException("Invalid segment properties info: missing or invalid ids (expected array)");
    }
    const auto& idsArr = idsIt->value().as_array();
    props->m_ids.reserve(idsArr.size());
    for (const auto& v : idsArr) {
      if (v.is_string()) {
        props->m_ids.push_back(parseUint64Base10(json::value_to<QString>(v)));
        continue;
      }
      if (v.is_int64()) {
        const int64_t vv = v.as_int64();
        if (vv < 0) {
          throw ZException("Invalid segment properties info: ids must not contain negative values");
        }
        props->m_ids.push_back(static_cast<uint64_t>(vv));
        continue;
      }
      if (v.is_uint64()) {
        props->m_ids.push_back(v.as_uint64());
        continue;
      }
      throw ZException("Invalid segment properties info: ids must contain strings or integers");
    }

    // Determine whether ids are strictly increasing (common case).
    props->m_idsSorted = std::is_sorted(props->m_ids.begin(), props->m_ids.end());
    if (!props->m_idsSorted) {
      props->m_sortedIds.resize(props->m_ids.size());
      for (size_t i = 0; i < props->m_ids.size(); ++i) {
        if (i > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
          throw ZException("Segment properties id list is too large");
        }
        props->m_sortedIds[i] = IdIndex{props->m_ids[i], static_cast<uint32_t>(i)};
      }
      std::sort(props->m_sortedIds.begin(), props->m_sortedIds.end(), [](const IdIndex& a, const IdIndex& b) {
        return a.id < b.id;
      });
    }

    // properties
    auto propsIt = inl.find("properties");
    if (propsIt == inl.end() || !propsIt->value().is_array()) {
      throw ZException("Invalid segment properties info: missing or invalid properties (expected array)");
    }
    const auto& propsArr = propsIt->value().as_array();

    props->m_properties.clear();
    props->m_properties.reserve(propsArr.size());

    bool seenLabel = false;
    bool seenDescription = false;
    bool seenTags = false;

    for (const auto& pv : propsArr) {
      if (!pv.is_object()) {
        throw ZException("Invalid segment properties info: properties elements must be objects");
      }
      const auto& po = pv.as_object();

      Property p{};
      p.id = requireString(po, "id");
      p.type = parsePropertyType(requireString(po, "type"));

      const auto descOpt = optionalString(po, "description");
      if (p.type == PropertyType::Tags) {
        if (descOpt) {
          throw ZException("Invalid segment properties info: tags property must not have 'description'");
        }
      } else {
        if (descOpt) {
          p.description = *descOpt;
        }
      }

      if (p.type == PropertyType::Tags) {
        p.tags = requireStringArray(po, "tags");
        auto tagDescIt = po.find("tag_descriptions");
        if (tagDescIt != po.end() && !tagDescIt->value().is_null()) {
          if (!tagDescIt->value().is_array()) {
            throw ZException("Invalid segment properties info: tag_descriptions must be an array");
          }
          const auto& tdArr = tagDescIt->value().as_array();
          if (tdArr.size() != p.tags.size()) {
            throw ZException("Invalid segment properties info: tag_descriptions must match tags length");
          }
          p.tagDescriptions.reserve(tdArr.size());
          for (const auto& td : tdArr) {
            if (!td.is_string()) {
              throw ZException("Invalid segment properties info: tag_descriptions must contain strings");
            }
            p.tagDescriptions.push_back(json::value_to<QString>(td));
          }
        }
      } else {
        if (po.find("tags") != po.end() || po.find("tag_descriptions") != po.end()) {
          throw ZException("Invalid segment properties info: only tags properties may contain 'tags' or 'tag_descriptions'");
        }
      }

      if (p.type == PropertyType::Number) {
        p.numberDataType = parseNumberDataType(requireString(po, "data_type"));
      } else {
        if (po.find("data_type") != po.end()) {
          throw ZException("Invalid segment properties info: only number properties may contain 'data_type'");
        }
      }

      auto valuesIt = po.find("values");
      if (valuesIt == po.end() || !valuesIt->value().is_array()) {
        throw ZException("Invalid segment properties info: missing or invalid property.values (expected array)");
      }
      const auto& valuesArr = valuesIt->value().as_array();
      if (valuesArr.size() != props->m_ids.size()) {
        throw ZException(fmt::format("Invalid segment properties info: property.values length mismatch (got {}, expected {})",
                                     valuesArr.size(),
                                     props->m_ids.size()));
      }

      if (p.type == PropertyType::Label || p.type == PropertyType::Description || p.type == PropertyType::String) {
        p.stringValues.reserve(valuesArr.size());
        for (const auto& vv : valuesArr) {
          if (!vv.is_string()) {
            throw ZException("Invalid segment properties info: string-like property.values must contain strings");
          }
          p.stringValues.push_back(json::value_to<QString>(vv));
        }
      } else if (p.type == PropertyType::Number) {
        p.numberValues.reserve(valuesArr.size());
        for (const auto& vv : valuesArr) {
          if (vv.is_double()) {
            p.numberValues.push_back(vv.as_double());
          } else if (vv.is_int64()) {
            p.numberValues.push_back(static_cast<double>(vv.as_int64()));
          } else if (vv.is_uint64()) {
            p.numberValues.push_back(static_cast<double>(vv.as_uint64()));
          } else {
            throw ZException("Invalid segment properties info: number property.values must contain numbers");
          }
        }
      } else if (p.type == PropertyType::Tags) {
        p.tagIndicesValues.resize(valuesArr.size());
        for (size_t i = 0; i < valuesArr.size(); ++i) {
          const auto& vv = valuesArr[i];
          if (vv.is_null()) {
            // Treat explicit null as an empty tag set (robustness for some producers).
            continue;
          }
          if (!vv.is_array()) {
            throw ZException("Invalid segment properties info: tags property.values elements must be arrays");
          }
          const auto& tagIdxArr = vv.as_array();
          auto& outIdx = p.tagIndicesValues[i];
          outIdx.reserve(tagIdxArr.size());
          uint64_t prev = 0;
          bool first = true;
          for (const auto& tv : tagIdxArr) {
            if (!tv.is_int64() && !tv.is_uint64()) {
              throw ZException("Invalid segment properties info: tags property.values elements must contain integers");
            }
            const uint64_t idx = tv.is_int64() ? static_cast<uint64_t>(tv.as_int64()) : tv.as_uint64();
            if (idx >= p.tags.size()) {
              throw ZException("Invalid segment properties info: tags property.values contains out-of-range index");
            }
            if (!first && idx < prev) {
              throw ZException("Invalid segment properties info: tags property.values indices must be in increasing order");
            }
            first = false;
            prev = idx;
            outIdx.push_back(static_cast<uint32_t>(idx));
          }
        }
      }

      if (p.type == PropertyType::Label) {
        if (seenLabel) {
          throw ZException("Invalid segment properties info: multiple label properties");
        }
        seenLabel = true;
        props->m_labelPropIndex = props->m_properties.size();
      } else if (p.type == PropertyType::Description) {
        if (seenDescription) {
          throw ZException("Invalid segment properties info: multiple description properties");
        }
        seenDescription = true;
        props->m_descriptionPropIndex = props->m_properties.size();
      } else if (p.type == PropertyType::Tags) {
        if (seenTags) {
          throw ZException("Invalid segment properties info: multiple tags properties");
        }
        seenTags = true;
        props->m_tagsPropIndex = props->m_properties.size();
      }

      props->m_properties.push_back(std::move(p));
    }
  };

  // Preferred form: `inline` object.
  auto inlineIt = root.find("inline");
  if (inlineIt != root.end() && !inlineIt->value().is_null()) {
    if (!inlineIt->value().is_object()) {
      throw ZException("Invalid segment properties info: 'inline' must be an object or null");
    }
    parseInlineObject(inlineIt->value().as_object());
    return props;
  }

  // Compatibility / robustness: some producers omit the `inline` wrapper and put `ids`/`properties`
  // at the root. Neuroglancer itself only documents the `inline` form, but this makes Atlas more
  // tolerant without changing semantics.
  const bool hasLegacyIds = root.find("ids") != root.end();
  const bool hasLegacyProps = root.find("properties") != root.end();
  if (hasLegacyIds || hasLegacyProps) {
    parseInlineObject(root);
    return props;
  }

  // No inline data. Treat as empty only if there are no other keys besides "@type" and optional `inline: null`.
  for (const auto& kv : root) {
    const std::string_view key(kv.key_c_str());
    if (key == "@type" || key == "inline") {
      continue;
    }
    // There are additional keys beyond the documented inline representation, but no inline data we can parse;
    // this likely indicates a newer representation that Atlas does not yet implement.
    throw ZException(fmt::format("Unsupported neuroglancer segment_properties format (no inline data). Keys: [{}]",
                                 joinKeys(root)));
  }

  return props;
}

std::optional<size_t> ZNeuroglancerPrecomputedSegmentProperties::findIndex(uint64_t id) const
{
  if (m_ids.empty()) {
    return std::nullopt;
  }

  if (m_idsSorted) {
    auto it = std::lower_bound(m_ids.begin(), m_ids.end(), id);
    if (it == m_ids.end() || *it != id) {
      return std::nullopt;
    }
    return static_cast<size_t>(it - m_ids.begin());
  }

  auto it = std::lower_bound(m_sortedIds.begin(), m_sortedIds.end(), id, [](const IdIndex& a, uint64_t b) {
    return a.id < b;
  });
  if (it == m_sortedIds.end() || it->id != id) {
    return std::nullopt;
  }
  return static_cast<size_t>(it->index);
}

std::optional<QString> ZNeuroglancerPrecomputedSegmentProperties::labelForId(uint64_t id) const
{
  if (!m_labelPropIndex) {
    return std::nullopt;
  }
  const auto idxOpt = findIndex(id);
  if (!idxOpt) {
    return std::nullopt;
  }
  const auto& p = m_properties.at(*m_labelPropIndex);
  CHECK(p.type == PropertyType::Label);
  CHECK(p.stringValues.size() == m_ids.size());
  return p.stringValues[*idxOpt];
}

std::optional<QString> ZNeuroglancerPrecomputedSegmentProperties::descriptionForId(uint64_t id) const
{
  if (!m_descriptionPropIndex) {
    return std::nullopt;
  }
  const auto idxOpt = findIndex(id);
  if (!idxOpt) {
    return std::nullopt;
  }
  const auto& p = m_properties.at(*m_descriptionPropIndex);
  CHECK(p.type == PropertyType::Description);
  CHECK(p.stringValues.size() == m_ids.size());
  return p.stringValues[*idxOpt];
}

QStringList ZNeuroglancerPrecomputedSegmentProperties::tagsForId(uint64_t id) const
{
  if (!m_tagsPropIndex) {
    return {};
  }
  const auto idxOpt = findIndex(id);
  if (!idxOpt) {
    return {};
  }

  const auto& p = m_properties.at(*m_tagsPropIndex);
  CHECK(p.type == PropertyType::Tags);
  CHECK(p.tagIndicesValues.size() == m_ids.size());

  QStringList out;
  const auto& indices = p.tagIndicesValues[*idxOpt];
  out.reserve(static_cast<int>(indices.size()));
  for (const uint32_t i : indices) {
    if (i >= p.tags.size()) {
      continue;
    }
    out.push_back(p.tags[i]);
  }
  return out;
}

} // namespace nim
