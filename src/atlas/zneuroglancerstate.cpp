#include "zneuroglancerstate.h"

#include "zneuroglancerremotecontext.h"
#include "zneuroglancerurl.h"
#include "zexception.h"
#include "zjson.h"
#include "zlog.h"

#include <boost/json.hpp>
#include <folly/coro/BlockingWait.h>

#include <algorithm>
#include <set>

#include <QByteArray>
#include <QUrl>

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

[[nodiscard]] std::optional<QString> asQStringIfScalar(const json::value& v)
{
  if (v.is_string()) {
    return json::value_to<QString>(v).trimmed();
  }
  if (v.is_int64()) {
    return QString::number(v.as_int64());
  }
  if (v.is_uint64()) {
    return QString::number(v.as_uint64());
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<uint64_t> parseUint64Base10(const QString& text)
{
  const QString trimmed = text.trimmed();
  if (trimmed.isEmpty()) {
    return std::nullopt;
  }

  bool ok = false;
  const uint64_t value = trimmed.toULongLong(&ok, 10);
  if (!ok) {
    return std::nullopt;
  }
  return value;
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
  if (!v.is_number()) {
    return std::nullopt;
  }
  return v.to_number<double>();
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
    const auto decodedOpt = decodeSupportedNeuroglancerPrecomputedSourceUrl(u0);
    if (!decodedOpt) {
      continue;
    }
    const QString u = decodedOpt->trimmed();
    // Exclude explicit non-volume subtypes like precomputed mesh sources.
    if (u.contains("#type=", Qt::CaseInsensitive)) {
      continue;
    }
    return u;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<QString> pickSkeletonSourceOverride(const QStringList& urls, const QString& volumeUrl)
{
  for (const QString& u0 : urls) {
    const QString u = normalizeNeuroglancerUrlDropFragment(u0);
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
      return normalizeNeuroglancerUrlDropFragment(u);
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

[[nodiscard]] std::optional<QString> tryExtractJsonFromUrlFragment(QString urlText)
{
  const QUrl url(urlText.trimmed());
  if (!url.isValid()) {
    return std::nullopt;
  }
  QString fragment = url.fragment(QUrl::FullyDecoded).trimmed();
  if (fragment.isEmpty()) {
    return std::nullopt;
  }
  if (fragment.startsWith('!')) {
    fragment = fragment.mid(1).trimmed();
  }
  if (!fragment.startsWith('{') && !fragment.startsWith('[') && fragment.contains('%')) {
    fragment = QString::fromUtf8(QByteArray::fromPercentEncoding(fragment.toUtf8())).trimmed();
  }
  if (fragment.startsWith('{') || fragment.startsWith('[')) {
    return fragment;
  }
  return std::nullopt;
}

[[nodiscard]] std::vector<ZNeuroglancerState::Layer::Segment> parseSegments(const json::object& layer)
{
  std::vector<ZNeuroglancerState::Layer::Segment> out;
  auto it = layer.find("segments");
  if (it == layer.end() || !it->value().is_array()) {
    return out;
  }

  const auto& arr = it->value().as_array();
  out.reserve(arr.size());
  for (const auto& entry : arr) {
    auto textOpt = asQStringIfScalar(entry);
    if (!textOpt) {
      continue;
    }
    QString text = textOpt->trimmed();
    bool visible = true;
    if (text.startsWith('!')) {
      visible = false;
      text = text.mid(1).trimmed();
    }
    const auto idOpt = parseUint64Base10(text);
    if (!idOpt) {
      continue;
    }
    ZNeuroglancerState::Layer::Segment segment;
    segment.id = *idOpt;
    segment.visible = visible;
    out.push_back(segment);
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

  if (auto selectedLayerIt = root.find("selectedLayer");
      selectedLayerIt != root.end() && selectedLayerIt->value().is_object()) {
    const auto& selectedObj = selectedLayerIt->value().as_object();
    if (auto layerNameIt = selectedObj.find("layer"); layerNameIt != selectedObj.end()) {
      if (const auto selectedName = asQStringIfString(layerNameIt->value())) {
        out.selectedLayerName = *selectedName;
      }
    }
  }

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
        l.segments = parseSegments(layer);

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

ZNeuroglancerState::InputParseResult
ZNeuroglancerState::parseInputText(const QString& text,
                                   std::chrono::milliseconds timeout,
                                   std::shared_ptr<const ZRemoteObjectStore> objectStore)
{
  InputParseResult out;
  const QString trimmed = text.trimmed();
  if (trimmed.isEmpty()) {
    return out;
  }

  const auto remoteContext = ZNeuroglancerRemoteContext::create(timeout, std::move(objectStore));

  try {
    if (trimmed.startsWith('{') || trimmed.startsWith('[')) {
      out.stateJson = boost::json::parse(trimmed.toStdString());
      out.status = InputStatus::Parsed;
      return out;
    }

    if (const auto fragmentJson = tryExtractJsonFromUrlFragment(trimmed)) {
      out.stateJson = boost::json::parse(fragmentJson->toStdString());
      out.status = InputStatus::Parsed;
      return out;
    }

    if (trimmed.contains("://") || trimmed.startsWith("gs://", Qt::CaseInsensitive) ||
        trimmed.startsWith("s3://", Qt::CaseInsensitive)) {
      const QString url = mapCloudStorageUrlToHttps(trimmed);
      auto responseOpt = folly::coro::blockingWait(remoteContext->getResponseAsync(url.toStdString()));
      if (!responseOpt) {
        out.status = InputStatus::Error;
        out.error = QStringLiteral("Failed to fetch Neuroglancer state JSON (HTTP 403/404):\n%1").arg(url);
        return out;
      }
      if (responseOpt->status != 200) {
        out.status = InputStatus::Error;
        out.error = QStringLiteral("Failed to fetch Neuroglancer state JSON:\n%1\n\nHTTP status: %2")
                      .arg(url)
                      .arg(responseOpt->status);
        return out;
      }

      const std::string jsonText(reinterpret_cast<const char*>(responseOpt->body.data()), responseOpt->body.size());
      out.stateJson = boost::json::parse(jsonText);
      out.status = InputStatus::Parsed;
      return out;
    }
  }
  catch (const std::exception& e) {
    out.status = InputStatus::Error;
    out.error = QStringLiteral("Failed to parse Neuroglancer state JSON:\n%1").arg(e.what());
    return out;
  }

  out.status = InputStatus::NotRecognized;
  return out;
}

} // namespace nim
