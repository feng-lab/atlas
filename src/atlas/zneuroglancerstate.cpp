#include "zneuroglancerstate.h"

#include "zexception.h"
#include "zjson.h"
#include "zlog.h"

#include <boost/json.hpp>

#include <algorithm>
#include <set>

namespace nim {
namespace json = boost::json;

namespace {

[[nodiscard]] std::optional<QString> asQStringIfString(const json::value& v)
{
  if (!v.is_string()) {
    return std::nullopt;
  }
  return json::value_to<QString>(v).trimmed();
}

[[nodiscard]] bool asBoolOrDefault(const json::object& o, const char* key, bool defaultValue)
{
  auto it = o.find(key);
  if (it == o.end() || it->value().is_null()) {
    return defaultValue;
  }
  if (!it->value().is_bool()) {
    return defaultValue;
  }
  return it->value().as_bool();
}

[[nodiscard]] std::optional<double> asDoubleIfNumber(const json::object& o, const char* key)
{
  auto it = o.find(key);
  if (it == o.end() || it->value().is_null()) {
    return std::nullopt;
  }
  const json::value& v = it->value();
  if (v.is_double()) {
    return v.as_double();
  }
  if (v.is_int64()) {
    return static_cast<double>(v.as_int64());
  }
  if (v.is_uint64()) {
    return static_cast<double>(v.as_uint64());
  }
  return std::nullopt;
}

void collectSourceUrls(const json::value& v, QStringList* out)
{
  CHECK(out);
  if (v.is_string()) {
    out->push_back(json::value_to<QString>(v).trimmed());
    return;
  }
  if (v.is_object()) {
    const auto& o = v.as_object();
    if (auto urlIt = o.find("url"); urlIt != o.end() && urlIt->value().is_string()) {
      out->push_back(json::value_to<QString>(urlIt->value()).trimmed());
    } else if (auto srcIt = o.find("source"); srcIt != o.end()) {
      // Some layer JSON uses nested {"source": ...} forms; recurse conservatively.
      collectSourceUrls(srcIt->value(), out);
    }
    return;
  }
  if (v.is_array()) {
    for (const auto& e : v.as_array()) {
      collectSourceUrls(e, out);
    }
  }
}

[[nodiscard]] std::optional<QString> pickPrecomputedVolumeUrl(const QStringList& urls)
{
  for (const QString& u0 : urls) {
    const QString u = u0.trimmed();
    if (!u.startsWith("precomputed://", Qt::CaseInsensitive)) {
      continue;
    }
    // Exclude explicit non-volume subtypes like precomputed mesh sources.
    if (u.contains("#type=", Qt::CaseInsensitive)) {
      continue;
    }
    return u;
  }
  return std::nullopt;
}

[[nodiscard]] QString normalizePrecomputedUrlDropFragment(QString url)
{
  QString s = url.trimmed();
  const int hash = s.indexOf('#');
  if (hash >= 0) {
    s = s.left(hash);
  }
  return s.trimmed();
}

[[nodiscard]] std::optional<QString> pickSkeletonSourceOverride(const QStringList& urls, const QString& volumeUrl)
{
  for (const QString& u0 : urls) {
    const QString u = normalizePrecomputedUrlDropFragment(u0);
    if (u.isEmpty() || u == volumeUrl) {
      continue;
    }
    if (!u.startsWith("precomputed://", Qt::CaseInsensitive)) {
      continue;
    }
    // Heuristic: skeleton datasets are commonly named ".../skeletons..." or contain "skeleton".
    if (u.contains("skeleton", Qt::CaseInsensitive)) {
      return u;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<QString> pickMeshSourceOverride(const QStringList& urls)
{
  for (const QString& u0 : urls) {
    const QString u = u0.trimmed();
    if (!u.startsWith("precomputed://", Qt::CaseInsensitive)) {
      continue;
    }
    // Neuroglancer mesh sources are often encoded as "precomputed://.../mesh#type=mesh".
    if (u.contains("#type=mesh", Qt::CaseInsensitive)) {
      return normalizePrecomputedUrlDropFragment(u);
    }
  }
  return std::nullopt;
}

[[nodiscard]] QStringList linkedSegmentationLayerNamesFromJson(const json::object& o)
{
  QStringList out;
  auto it = o.find("linkedSegmentationLayer");
  if (it == o.end() || it->value().is_null()) {
    return out;
  }
  if (!it->value().is_object()) {
    return out;
  }
  const auto& mapObj = it->value().as_object();
  std::set<QString> uniq;
  for (const auto& kv : mapObj) {
    (void)kv.key();
    if (!kv.value().is_string()) {
      continue;
    }
    const QString name = json::value_to<QString>(kv.value()).trimmed();
    if (!name.isEmpty()) {
      uniq.insert(name);
    }
  }
  for (const auto& s : uniq) {
    out.push_back(s);
  }
  return out;
}

} // namespace

ZNeuroglancerState::ParseResult ZNeuroglancerState::parse(const json::value& stateJson)
{
  ParseResult out;

  if (!stateJson.is_object()) {
    out.warnings << QStringLiteral("Neuroglancer state JSON must be an object.");
    return out;
  }
  const auto& root = stateJson.as_object();

  auto layersIt = root.find("layers");
  if (layersIt == root.end()) {
    out.warnings << QStringLiteral("Neuroglancer state JSON has no 'layers' field.");
    return out;
  }
  if (!layersIt->value().is_array()) {
    out.warnings << QStringLiteral("Neuroglancer state JSON 'layers' must be an array.");
    return out;
  }

  const auto& layers = layersIt->value().as_array();
  out.layers.reserve(layers.size());

  for (const auto& layerV : layers) {
    if (!layerV.is_object()) {
      continue;
    }
    const auto& layer = layerV.as_object();

    const auto typeIt = layer.find("type");
    if (typeIt == layer.end() || !typeIt->value().is_string()) {
      continue;
    }
    const QString type = json::value_to<QString>(typeIt->value()).trimmed().toLower();

    // Capture optional per-layer metadata.
    QString layerName;
    if (auto nameIt = layer.find("name"); nameIt != layer.end()) {
      if (auto s = asQStringIfString(nameIt->value())) {
        layerName = *s;
      }
    }
    const bool visible = asBoolOrDefault(layer, "visible", /*default=*/true);
    const std::optional<double> opacity = asDoubleIfNumber(layer, "opacity");

    // Collect all source URL strings we can find under the "source" field.
    QStringList sourceUrls;
    if (auto srcIt = layer.find("source"); srcIt != layer.end()) {
      collectSourceUrls(srcIt->value(), &sourceUrls);
    }

    if (type == "image" || type == "segmentation") {
      const auto volUrlOpt = pickPrecomputedVolumeUrl(sourceUrls);
      if (!volUrlOpt) {
        const QString what = layerName.isEmpty() ? QStringLiteral("<unnamed>") : layerName;
        out.warnings << QStringLiteral("Skipping %1 layer '%2': no supported precomputed volume source found.")
                           .arg(type)
                           .arg(what);
        continue;
      }

      Layer l;
      l.type = (type == "segmentation") ? LayerType::Segmentation : LayerType::Image;
      l.name = layerName;
      l.volumeUrl = *volUrlOpt;
      l.visible = visible;
      l.opacity = opacity;

      if (l.type == LayerType::Segmentation) {
        // Opportunistically capture external sources from the same layer.
        if (const auto skelOpt = pickSkeletonSourceOverride(sourceUrls, l.volumeUrl)) {
          l.skeletonSourceOverrideUrl = *skelOpt;
        }
        if (const auto meshOpt = pickMeshSourceOverride(sourceUrls)) {
          l.meshSourceOverrideUrl = *meshOpt;
        }
      }

      out.layers.push_back(std::move(l));
      continue;
    }

    if (type == "annotation") {
      const auto annUrlOpt = pickPrecomputedVolumeUrl(sourceUrls);
      if (annUrlOpt) {
        AnnotationsBinding b;
        b.annotationsRootUrl = *annUrlOpt;
        b.linkedSegmentationLayerNames = linkedSegmentationLayerNamesFromJson(layer);
        if (!b.annotationsRootUrl.isEmpty() && !b.linkedSegmentationLayerNames.isEmpty()) {
          out.annotationsBindings.push_back(std::move(b));
        }
      }
      // Do not add warnings for annotation layers; they are intentionally not opened in this pass.
      continue;
    }

    // Ignore unsupported layer types, but leave a lightweight warning to aid debugging.
    if (!type.isEmpty()) {
      out.warnings << QStringLiteral("Ignoring unsupported Neuroglancer layer type '%1'.").arg(type);
    }
  }

  return out;
}

} // namespace nim
