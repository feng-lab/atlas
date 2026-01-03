#include "zneuroglancerprecomputeddatasetlist.h"

#include "zlog.h"
#include "zsysteminfo.h"

#include <QFileInfo>

#include <algorithm>
#include <set>

namespace nim {

namespace {

constexpr int kListVersion = 1;

[[nodiscard]] std::vector<ZNeuroglancerPrecomputedDatasetList::Entry> defaultExampleList()
{
  std::vector<ZNeuroglancerPrecomputedDatasetList::Entry> out;

  // From Neuroglancer README examples.
  out.push_back(
    {"Janelia FlyEM FIB-25 (image)", "image", "precomputed://gs://neuroglancer-public-data/flyem_fib-25/image"});
  out.push_back({"Janelia FlyEM FIB-25 (ground truth segmentation)",
                 "segmentation",
                 "precomputed://gs://neuroglancer-public-data/flyem_fib-25/ground_truth"});
  out.push_back({"Kasthuri 2011 (image)", "image", "precomputed://gs://neuroglancer-public-data/kasthuri2011/image"});
  out.push_back({"Kasthuri 2011 (color corrected image)",
                 "image",
                 "precomputed://gs://neuroglancer-public-data/kasthuri2011/image_color_corrected"});
  out.push_back({"Kasthuri 2011 (ground truth segmentation)",
                 "segmentation",
                 "precomputed://gs://neuroglancer-public-data/kasthuri2011/ground_truth"});

  // From viewer state: gs://neuroglancer-janelia-flyem-hemibrain/v1.0/neuroglancer_demo_states/base.json
  out.push_back({"FlyEM Hemibrain v1.0 (emdata image, CLAHE YZ JPEG)",
                 "image",
                 "precomputed://gs://neuroglancer-janelia-flyem-hemibrain/emdata/clahe_yz/jpeg"});
  out.push_back({"FlyEM Hemibrain v1.0 (segmentation)",
                 "segmentation",
                 "precomputed://gs://neuroglancer-janelia-flyem-hemibrain/v1.0/segmentation"});
  out.push_back({"FlyEM Hemibrain v1.0 (ROIs segmentation)",
                 "segmentation",
                 "precomputed://gs://neuroglancer-janelia-flyem-hemibrain/v1.0/rois"});
  out.push_back({"FlyEM Hemibrain v1.0 (mitochondria segmentation)",
                 "segmentation",
                 "precomputed://gs://neuroglancer-janelia-flyem-hemibrain/mito_20190717.27250582"});
  out.push_back({"FlyEM Hemibrain v1.0 (mask segmentation)",
                 "segmentation",
                 "precomputed://gs://neuroglancer-janelia-flyem-hemibrain/mask_normalized_round6"});

  // From viewer state: gs://fafb-ffn1/main_ng.json
  out.push_back(
    {"FAFB v14 (original image)", "image", "precomputed://gs://neuroglancer-fafb-data/fafb_v14/fafb_v14_orig"});
  out.push_back({"FAFB v14 (CLAHE image)", "image", "precomputed://gs://neuroglancer-fafb-data/fafb_v14/fafb_v14_clahe"});
  out.push_back({"FAFB-FFN1 (2019-08-05 segmentation)", "segmentation", "precomputed://gs://fafb-ffn1-20190805/segmentation"});

  return out;
}

} // namespace

QString ZNeuroglancerPrecomputedDatasetList::examplesFilename()
{
  return QStringLiteral("neuroglancer_precomputed_examples.json");
}

QString ZNeuroglancerPrecomputedDatasetList::userHistoryFilename()
{
  return QStringLiteral("neuroglancer_precomputed_history.json");
}

QString ZNeuroglancerPrecomputedDatasetList::examplesFilePath()
{
  return ZSystemInfo::resourcesDir().absoluteFilePath(examplesFilename());
}

QString ZNeuroglancerPrecomputedDatasetList::userHistoryFilePath()
{
  return ZSystemInfo::configDir().absoluteFilePath(userHistoryFilename());
}

std::vector<ZNeuroglancerPrecomputedDatasetList::Entry> ZNeuroglancerPrecomputedDatasetList::loadExamples(QString* errorMsg)
{
  QString err;
  std::vector<Entry> entries;
  {
    const QFileInfo fi(examplesFilePath());
    if (fi.exists()) {
      try {
        const auto jo = loadJsonObject(examplesFilePath());
        entries = parseListObject(jo, &err);
      }
      catch (const std::exception& e) {
        err = QString("Failed to parse %1: %2").arg(examplesFilePath()).arg(QString::fromUtf8(e.what()));
      }
    }
  }
  if (!err.isEmpty()) {
    LOG(WARNING) << "Failed to load Neuroglancer examples list: " << err.toStdString();
  }
  if (entries.empty()) {
    entries = defaultExampleList();
  }
  normalizeAndDeduplicate(&entries);
  if (errorMsg) {
    errorMsg->clear();
  }
  return entries;
}

std::vector<ZNeuroglancerPrecomputedDatasetList::Entry> ZNeuroglancerPrecomputedDatasetList::loadUserHistory(QString* errorMsg)
{
  if (errorMsg) {
    errorMsg->clear();
  }

  std::vector<Entry> entries;
  const QFileInfo fi(userHistoryFilePath());
  if (fi.exists()) {
    try {
      const auto jo = loadJsonObject(userHistoryFilePath());
      entries = parseListObject(jo, errorMsg);
    }
    catch (const std::exception& e) {
      if (errorMsg) {
        *errorMsg = QString("Failed to parse %1: %2").arg(userHistoryFilePath()).arg(QString::fromUtf8(e.what()));
      }
    }
  }
  normalizeAndDeduplicate(&entries);
  return entries;
}

bool ZNeuroglancerPrecomputedDatasetList::saveUserHistory(const std::vector<Entry>& entries, QString* errorMsg)
{
  if (errorMsg) {
    errorMsg->clear();
  }

  try {
    // Normalize/dedupe before write to keep the file stable even if the UI model contains duplicates.
    std::vector<Entry> normalized = entries;
    normalizeAndDeduplicate(&normalized);

    const QString outPath = userHistoryFilePath();
    const json::object jo = toListObject(normalized);
    saveJsonObject(jo, outPath);
    return true;
  }
  catch (const std::exception& e) {
    if (errorMsg) {
      *errorMsg = QString("Failed to write %1: %2").arg(userHistoryFilePath()).arg(QString::fromUtf8(e.what()));
    }
    return false;
  }
}

void ZNeuroglancerPrecomputedDatasetList::normalizeAndDeduplicate(std::vector<Entry>* entries)
{
  CHECK(entries);
  std::vector<Entry> out;
  out.reserve(entries->size());

  std::set<QString> seen;
  for (auto& e : *entries) {
    const QString url = normalizedUrl(e.url);
    if (url.isEmpty()) {
      continue;
    }
    if (seen.contains(url)) {
      continue;
    }
    seen.insert(url);
    Entry clean;
    clean.url = url;
    clean.name = e.name.trimmed();
    clean.kind = e.kind.trimmed();
    out.push_back(std::move(clean));
  }

  *entries = std::move(out);
}

void ZNeuroglancerPrecomputedDatasetList::upsertMostRecent(std::vector<Entry>* entries, Entry entry)
{
  CHECK(entries);

  entry.url = normalizedUrl(std::move(entry.url));
  entry.name = entry.name.trimmed();
  entry.kind = entry.kind.trimmed();
  if (entry.url.isEmpty()) {
    return;
  }

  // Remove existing entries with the same URL (case-sensitive exact match after normalization).
  entries->erase(std::remove_if(entries->begin(),
                                entries->end(),
                                [&](const Entry& e) { return normalizedUrl(e.url) == entry.url; }),
                 entries->end());
  entries->insert(entries->begin(), std::move(entry));
}

QString ZNeuroglancerPrecomputedDatasetList::normalizedUrl(QString url)
{
  QString u = url.trimmed();
  if (u.endsWith("/info", Qt::CaseInsensitive)) {
    u.chop(5);
  }
  while (u.endsWith('/') && u.size() > 1) {
    // Avoid stripping the second slash of schemes like "gs://" or "http://".
    if (u.size() >= 3 && u.endsWith("://")) {
      break;
    }
    u.chop(1);
  }
  return u.trimmed();
}

std::vector<ZNeuroglancerPrecomputedDatasetList::Entry> ZNeuroglancerPrecomputedDatasetList::parseListObject(const json::object& jo,
                                                                                                             QString* errorMsg)
{
  if (errorMsg) {
    errorMsg->clear();
  }

  if (jo.empty()) {
    return {};
  }

  auto it = jo.find("datasets");
  if (it == jo.end()) {
    if (errorMsg) {
      *errorMsg = QString("Invalid dataset list JSON: missing 'datasets' array");
    }
    return {};
  }
  if (!it->value().is_array()) {
    if (errorMsg) {
      *errorMsg = QString("Invalid dataset list JSON: 'datasets' must be an array");
    }
    return {};
  }

  std::vector<Entry> out;
  const auto& arr = it->value().as_array();
  out.reserve(arr.size());
  for (const auto& v : arr) {
    if (!v.is_object()) {
      continue;
    }
    const auto& o = v.as_object();

    auto urlIt = o.find("url");
    if (urlIt == o.end() || !urlIt->value().is_string()) {
      continue;
    }

    Entry e;
    e.url = json::value_to<QString>(urlIt->value());
    if (auto nameIt = o.find("name"); nameIt != o.end() && nameIt->value().is_string()) {
      e.name = json::value_to<QString>(nameIt->value());
    }
    if (auto kindIt = o.find("kind"); kindIt != o.end() && kindIt->value().is_string()) {
      e.kind = json::value_to<QString>(kindIt->value());
    }
    out.push_back(std::move(e));
  }

  return out;
}

json::object ZNeuroglancerPrecomputedDatasetList::toListObject(const std::vector<Entry>& entries)
{
  json::object out;
  out["version"] = kListVersion;

  json::array datasets;
  datasets.reserve(entries.size());
  for (const auto& e : entries) {
    json::object o;
    o["url"] = json::value_from(e.url);
    if (!e.name.trimmed().isEmpty()) {
      o["name"] = json::value_from(e.name.trimmed());
    }
    if (!e.kind.trimmed().isEmpty()) {
      o["kind"] = json::value_from(e.kind.trimmed());
    }
    datasets.push_back(std::move(o));
  }

  out["datasets"] = std::move(datasets);
  return out;
}

} // namespace nim
