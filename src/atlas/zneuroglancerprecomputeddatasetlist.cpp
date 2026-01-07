#include "zneuroglancerprecomputeddatasetlist.h"

#include "zlog.h"
#include "zsysteminfo.h"

#include <QFileInfo>

#include <algorithm>
#include <set>

namespace nim {

namespace {

constexpr int kListVersion = 1;

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

  // Load from the installed resources directory (allows patching examples without rebuilding).
  const QFileInfo diskFi(examplesFilePath());
  if (diskFi.exists()) {
    try {
      const auto jo = loadJsonObject(examplesFilePath());
      entries = parseListObject(jo, &err);
    }
    catch (const std::exception& e) {
      err = QString("Failed to parse %1: %2").arg(examplesFilePath()).arg(QString::fromUtf8(e.what()));
    }
  }

  if (entries.empty()) {
    if (!diskFi.exists()) {
      err = QString("Examples file not found: %1").arg(examplesFilePath());
    }
    if (!err.isEmpty()) {
      LOG(WARNING) << "Failed to load Neuroglancer examples list: " << err.toStdString();
    }
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

    auto trimOrEmpty = [](const QString& s) { return s.trimmed(); };

    Entry clean = e;
    clean.url = url;
    clean.name = trimOrEmpty(e.name);
    clean.kind = trimOrEmpty(e.kind);
    clean.meshSourceOverrideUrl = trimOrEmpty(e.meshSourceOverrideUrl);
    clean.skeletonSourceOverrideUrl = trimOrEmpty(e.skeletonSourceOverrideUrl);

    // If duplicates exist (e.g. manual edits), merge missing optional metadata into the first occurrence
    // to avoid losing overrides during normalization.
    if (seen.contains(url)) {
      auto it = std::find_if(out.begin(), out.end(), [&](const Entry& x) { return x.url == url; });
      if (it == out.end()) {
        continue;
      }
      if (it->name.isEmpty() && !clean.name.isEmpty()) {
        it->name = std::move(clean.name);
      }
      if (it->kind.isEmpty() && !clean.kind.isEmpty()) {
        it->kind = std::move(clean.kind);
      }
      if (it->meshSourceOverrideUrl.isEmpty() && !clean.meshSourceOverrideUrl.isEmpty()) {
        it->meshSourceOverrideUrl = std::move(clean.meshSourceOverrideUrl);
      }
      if (it->skeletonSourceOverrideUrl.isEmpty() && !clean.skeletonSourceOverrideUrl.isEmpty()) {
        it->skeletonSourceOverrideUrl = std::move(clean.skeletonSourceOverrideUrl);
      }
      continue;
    }

    seen.insert(url);
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
  entry.meshSourceOverrideUrl = entry.meshSourceOverrideUrl.trimmed();
  entry.skeletonSourceOverrideUrl = entry.skeletonSourceOverrideUrl.trimmed();
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

QString ZNeuroglancerPrecomputedDatasetList::normalizedUrlForMatch(QString url)
{
  return normalizedUrl(std::move(url));
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
    if (auto meshIt = o.find("mesh_source_override_url"); meshIt != o.end() && meshIt->value().is_string()) {
      e.meshSourceOverrideUrl = json::value_to<QString>(meshIt->value());
    }
    if (auto skelIt = o.find("skeleton_source_override_url"); skelIt != o.end() && skelIt->value().is_string()) {
      e.skeletonSourceOverrideUrl = json::value_to<QString>(skelIt->value());
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
    if (!e.meshSourceOverrideUrl.trimmed().isEmpty()) {
      o["mesh_source_override_url"] = json::value_from(e.meshSourceOverrideUrl.trimmed());
    }
    if (!e.skeletonSourceOverrideUrl.trimmed().isEmpty()) {
      o["skeleton_source_override_url"] = json::value_from(e.skeletonSourceOverrideUrl.trimmed());
    }
    datasets.push_back(std::move(o));
  }

  out["datasets"] = std::move(datasets);
  return out;
}

} // namespace nim
