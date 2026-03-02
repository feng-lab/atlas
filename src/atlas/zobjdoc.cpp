#include "zobjdoc.h"

#include "zdoc.h"
#include "zbenchtimer.h"
#include "zlog.h"
#include "zfileutils.h"
#include "zchooseobjdialog.h"
#include "zsysteminfo.h"
#include <folly/OperationCancelled.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/executors/GlobalExecutor.h>
#include <QMessageBox>
#include <QApplication>
#include <QFileInfo>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace nim {

namespace {

// Order-insensitive structural hash for boost::json::value.
// - Used only as a fast pre-bucket for dedup/alias grouping in ZObjDoc::read().
// - All candidate matches are still verified with deep (==) comparison, so
//   hash collisions are safe (may only reduce performance slightly).
static uint64_t mix64(uint64_t x)
{
  // splitmix64 finalizer
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

static uint64_t fnv1a64(std::string_view s)
{
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t hashJsonValue(const json::value& v)
{
  uint64_t h = mix64(static_cast<uint64_t>(v.kind()) + 0x9e3779b97f4a7c15ULL);
  switch (v.kind()) {
    case json::kind::null:
      return h;
    case json::kind::bool_:
      return mix64(h ^ static_cast<uint64_t>(v.as_bool()));
    case json::kind::int64:
      return mix64(h ^ static_cast<uint64_t>(v.as_int64()));
    case json::kind::uint64:
      return mix64(h ^ v.as_uint64());
    case json::kind::double_: {
      const double d = v.as_double();
      const uint64_t bits = std::bit_cast<uint64_t>(d);
      return mix64(h ^ bits);
    }
    case json::kind::string: {
      const auto& s = v.as_string();
      return mix64(h ^ fnv1a64(std::string_view(s.data(), s.size())));
    }
    case json::kind::array: {
      const auto& a = v.as_array();
      h ^= mix64(a.size());
      for (const auto& e : a) {
        // Order-dependent for arrays (JSON arrays are ordered).
        h = mix64(h ^ hashJsonValue(e));
      }
      return h;
    }
    case json::kind::object: {
      const auto& o = v.as_object();
      // JSON objects are treated as key/value maps; do an order-insensitive
      // combine so hashes remain stable regardless of insertion order.
      uint64_t acc = mix64(o.size());
      for (const auto& [k, vv] : o) {
        const uint64_t kh = fnv1a64(std::string_view(k.data(), k.size()));
        const uint64_t vh = hashJsonValue(vv);
        acc ^= mix64(kh ^ (vh + 0x9e3779b97f4a7c15ULL));
      }
      return mix64(h ^ acc);
    }
  }
  return h;
}

} // namespace

bool ZObjDoc::canPrepareLoadAsync([[maybe_unused]] const json::value& jValue) const
{
  return false;
}

folly::coro::Task<ZObjDoc::PreparedLoadResult>
ZObjDoc::prepareLoadAsync([[maybe_unused]] const json::value& jValue,
                          [[maybe_unused]] const AsyncLoadContext& ctx) const
{
  co_return PreparedLoadResult{};
}

ZObjDoc::ZObjDoc(ZDoc& doc)
  : QObject(&doc)
  , m_doc(doc)
{}

void ZObjDoc::showObjInGraphicalShell(size_t id) const
{
  ZFileUtils::showInGraphicalShell(objPath(id));
}

size_t ZObjDoc::chooseOneObjWithWidget(const QString& title, QWidget* parent) const
{
  if (hasObj()) {
    ZChooseObjDialog dlg(*this, false, parent);
    if (!title.isEmpty()) {
      dlg.setWindowTitle(title);
    }
    if (dlg.exec() == QDialog::Accepted) {
      return dlg.selectedID();
    }
  }
  return 0;
}

QString ZObjDoc::objNameWithModifiedMarker(size_t id) const
{
  if (objHasUnsavedChange(id)) {
    return QString("%1*").arg(objName(id));
  }

  return objName(id);
}

QString ZObjDoc::objNameWithModifiedMarkerAndID(size_t id) const
{
  if (objHasUnsavedChange(id)) {
    return QString("%1* (id: %2)").arg(objName(id)).arg(id);
  }

  return QString("%1 (id: %2)").arg(objName(id)).arg(id);
}

std::map<size_t, size_t> ZObjDoc::read(const std::vector<std::pair<QString, json::value>>& docKeyValueList,
                                       QString& err)
{
  ZBenchTimer bt(fmt::format("ZObjDoc::read('{}')", typeName().toStdString()));
  std::map<size_t, size_t> idmap;

  std::map<size_t, json::value> idToJsonValue;
  for (const auto& keyValue : docKeyValueList) {
    QString keyString = keyValue.first;
    CHECK(keyString.startsWith(typeName()));
    bool ok = false;
    size_t id = 0;
    if (keyString.length() > typeName().length() + 1) {
      keyString.remove(0, typeName().length() + 1);
      if (keyString.trimmed().isEmpty()) {
        LOG(WARNING) << "Invalid object key " << keyValue.first;
        continue;
      }
      id = keyString.toLongLong(&ok);
    }
    if (ok && id > 0) {
      idToJsonValue[id] = keyValue.second;
    } else {
      LOG(WARNING) << "Invalid object key " << keyValue.first;
    }
  }
  // VLOG(1) << json::value_from(idToJsonValue);
  bt.recordEvent(fmt::format("parsed keys={} unique={}", docKeyValueList.size(), idToJsonValue.size()));

  struct JsonEntry
  {
    size_t id = 0;
    const json::value* value = nullptr;
    uint64_t hash = 0;
  };

  std::vector<JsonEntry> entries;
  entries.reserve(idToJsonValue.size());
  for (const auto& [id, value] : idToJsonValue) {
    entries.push_back(JsonEntry{id, &value, hashJsonValue(value)});
  }
  std::sort(entries.begin(), entries.end(), [](const JsonEntry& a, const JsonEntry& b) {
    if (a.hash != b.hash) {
      return a.hash < b.hash;
    }
    return a.id < b.id;
  });

  struct JsonGroup
  {
    const json::value* value = nullptr;
    std::vector<size_t> ids;
  };

  std::vector<JsonGroup> groups;
  groups.reserve(entries.size());
  for (size_t i = 0; i < entries.size();) {
    size_t j = i + 1;
    while (j < entries.size() && entries[j].hash == entries[i].hash) {
      ++j;
    }

    std::vector<JsonGroup> runGroups;
    runGroups.reserve(j - i);
    for (size_t k = i; k < j; ++k) {
      bool matched = false;
      for (auto& g : runGroups) {
        if (g.value && entries[k].value && *g.value == *entries[k].value) {
          g.ids.push_back(entries[k].id);
          matched = true;
          break;
        }
      }
      if (!matched) {
        JsonGroup g;
        g.value = entries[k].value;
        g.ids.push_back(entries[k].id);
        runGroups.push_back(std::move(g));
      }
    }

    for (auto& g : runGroups) {
      groups.push_back(std::move(g));
    }

    i = j;
  }

  std::sort(groups.begin(), groups.end(), [](const JsonGroup& a, const JsonGroup& b) {
    CHECK(!a.ids.empty() && !b.ids.empty());
    return a.ids.front() < b.ids.front();
  });

  size_t groupCount = groups.size();
  size_t maxGroupSize = 0;
  for (const auto& g : groups) {
    maxGroupSize = std::max(maxGroupSize, g.ids.size());
  }

  const QString docTypeName = typeName();
  const auto makeLoadLabel = [](const QString& docType, const json::value& jv) -> QString {
    auto firstStringFromArray = [](const json::value& v) -> QString {
      if (v.is_string()) {
        return json::value_to<QString>(v);
      }
      if (!v.is_array()) {
        return QString{};
      }
      const auto& a = v.as_array();
      if (a.empty() || !a.front().is_string()) {
        return QString{};
      }
      return json::value_to<QString>(a.front());
    };

    QString summary;
    if (jv.is_string()) {
      summary = json::value_to<QString>(jv);
    } else if (jv.is_object()) {
      const auto& jo = jv.as_object();

      // Prefer a path-like summary when available (common for image sources).
      if (auto it = jo.find("filenames"); it != jo.end()) {
        summary = firstStringFromArray(it->value());
        if (!summary.isEmpty() && it->value().is_array() && it->value().as_array().size() > 1) {
          summary = QString("%1 (x%2)").arg(summary).arg(it->value().as_array().size());
        }
      }
      if (summary.isEmpty()) {
        if (auto it = jo.find("Path"); it != jo.end()) { // legacy key for persisted image sources
          summary = firstStringFromArray(it->value());
          if (!summary.isEmpty() && it->value().is_array() && it->value().as_array().size() > 1) {
            summary = QString("%1 (x%2)").arg(summary).arg(it->value().as_array().size());
          }
        }
      }

      // Otherwise, fall back to type information.
      if (summary.isEmpty()) {
        if (auto it = jo.find("type"); it != jo.end() && it->value().is_string()) {
          const QString t = json::value_to<QString>(it->value()).trimmed();
          if (!t.isEmpty()) {
            summary = QString("type=%1").arg(t);
          }
        }
      }

      if (summary.isEmpty()) {
        summary = QString::fromUtf8(jsonTypeName(jv).c_str());
      }
    } else {
      summary = QString::fromUtf8(jsonTypeName(jv).c_str());
    }

    if (summary.trimmed().isEmpty()) {
      if (docType.trimmed().isEmpty()) {
        return QString::fromUtf8(jsonTypeName(jv).c_str());
      }
      return docType;
    }
    if (docType.trimmed().isEmpty()) {
      return summary;
    }
    return QString("%1 (%2)").arg(docType).arg(summary);
  };

  struct PreparedGroup
  {
    size_t groupIndex = 0;
    PreparedLoadResult prepared;
  };

  std::vector<std::optional<PreparedLoadResult>> prepared;
  prepared.resize(groups.size());
  std::vector<QString> groupLabels;
  groupLabels.resize(groups.size());
  for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
    const auto& group = groups[groupIndex];
    CHECK(group.value != nullptr);
    groupLabels[groupIndex] = makeLoadLabel(docTypeName, *group.value);
  }

  const std::vector<size_t> startingObjs = objs();
  std::vector<bool> groupHasStartingMatch(groups.size(), false);
  for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
    const auto& group = groups[groupIndex];
    CHECK(group.value != nullptr);
    const json::value& jv = *group.value;
    for (const size_t obj : startingObjs) {
      if (isSameObj(jv, jsonValue(obj))) {
        groupHasStartingMatch[groupIndex] = true;
        break;
      }
    }
  }

  // Prepare expensive loads in parallel (off the doc thread), then commit sequentially.
  // - Preparation is optional and doc-specific via canPrepareLoadAsync()/prepareLoadAsync().
  // - Committing remains single-threaded to preserve thread affinity and deterministic id allocation.
  const AsyncLoadContext asyncCtx{.commitThread = QThread::currentThread()};
  auto cpuExecutor = folly::getGlobalCPUExecutor();
  std::vector<folly::coro::TaskWithExecutor<PreparedGroup>> tasks;
  std::vector<size_t> taskGroupIndices;
  for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
    if (groupHasStartingMatch[groupIndex]) {
      continue;
    }
    const auto& group = groups[groupIndex];
    CHECK(group.value != nullptr);
    const json::value& jv = *group.value;
    if (!canPrepareLoadAsync(jv)) {
      continue;
    }

    const json::value* jvPtr = group.value;
    const QString groupLabel = groupLabels[groupIndex];
    taskGroupIndices.push_back(groupIndex);
    tasks.push_back(folly::coro::co_withExecutor(
      cpuExecutor,
      folly::coro::co_invoke([this, groupIndex, jvPtr, asyncCtx, groupLabel]() -> folly::coro::Task<PreparedGroup> {
        PreparedGroup out;
        out.groupIndex = groupIndex;
        try {
          out.prepared = co_await prepareLoadAsync(*jvPtr, asyncCtx);
        }
        catch (const folly::OperationCancelled&) {
          out.prepared.errorMsg = QString("%1: load cancelled").arg(groupLabel);
        }
        catch (const ZException& e) {
          out.prepared.errorMsg = QString("%1: %2").arg(groupLabel).arg(QString::fromUtf8(e.what()));
        }
        catch (const std::exception& e) {
          out.prepared.errorMsg = QString("%1: %2").arg(groupLabel).arg(QString::fromUtf8(e.what()));
        }
        catch (...) {
          out.prepared.errorMsg = QString("%1: Unknown exception while preparing load").arg(groupLabel);
        }
        co_return out;
      })));
  }

  if (!tasks.empty()) {
    auto results = folly::coro::blockingWait(folly::coro::collectAllTryRange(std::move(tasks)));
    CHECK(results.size() == taskGroupIndices.size());
    for (size_t taskIndex = 0; taskIndex < results.size(); ++taskIndex) {
      auto& r = results[taskIndex];
      const size_t groupIndex = taskGroupIndices[taskIndex];
      CHECK(groupIndex < groupLabels.size());
      const QString& groupLabel = groupLabels[groupIndex];
      if (r.hasException()) {
        // We should never get here because tasks catch all exceptions, but be defensive.
        try {
          std::rethrow_exception(r.exception().to_exception_ptr());
        }
        catch (const std::exception& e) {
          err += QString("%1: Async prepare task failed: %2\n").arg(groupLabel).arg(QString::fromUtf8(e.what()));
        }
        catch (...) {
          err += QString("%1: Async prepare task failed: unknown exception\n").arg(groupLabel);
        }
        continue;
      }
      PreparedGroup pg = std::move(r.value());
      CHECK(pg.groupIndex == groupIndex) << "Async prepare returned wrong groupIndex";
      CHECK(pg.groupIndex < prepared.size());
      prepared[pg.groupIndex] = std::move(pg.prepared);
    }
  }

  for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
    const auto& group = groups[groupIndex];
    CHECK(group.value != nullptr);
    const json::value& jv = *group.value;
    CHECK(groupIndex < groupLabels.size());
    const QString& groupLabel = groupLabels[groupIndex];
    const std::vector<size_t>& ids = group.ids;

    // Check existing objects, including those loaded earlier in this read() call.
    std::set<size_t> existingIds;
    for (const size_t obj : objs()) {
      if (isSameObj(jv, jsonValue(obj))) {
        existingIds.insert(obj);
      }
    }

    if (existingIds.empty()) {
      // Prefer prepared payload (if any), otherwise fall back to synchronous loadFile().
      if (prepared[groupIndex].has_value()) {
        PreparedLoadResult& pr = *prepared[groupIndex];
        if (!pr.errorMsg.isEmpty()) {
          err += QString("%1\n").arg(pr.errorMsg);
          continue;
        }
        if (pr.commit) {
          QString errMsg;
          size_t id = 0;
          try {
            id = pr.commit(errMsg);
          }
          catch (const folly::OperationCancelled&) {
            errMsg = QString("%1: load cancelled").arg(groupLabel);
          }
          catch (const ZException& e) {
            errMsg = QString("%1: %2").arg(groupLabel).arg(QString::fromUtf8(e.what()));
          }
          catch (const std::exception& e) {
            errMsg = QString("%1: %2").arg(groupLabel).arg(QString::fromUtf8(e.what()));
          }
          catch (...) {
            errMsg = QString("%1: Unknown exception while committing load").arg(groupLabel);
          }
          if (id == 0) {
            if (errMsg.trimmed().isEmpty()) {
              errMsg = QString("%1: Failed to commit prepared load").arg(groupLabel);
            }
            err += QString("%1\n").arg(errMsg);
            continue;
          }
          existingIds.insert(id);
        }
      }

      if (existingIds.empty()) {
        QString errMsg;
        size_t id = loadFile(jv, errMsg);
        if (id == 0) {
          err += QString("%1\n").arg(errMsg);
          continue;
        }
        existingIds.insert(id);
      }
    }

    if (ids.size() > existingIds.size()) {
      size_t firstId = *existingIds.begin();
      size_t num = ids.size() - existingIds.size();
      for (size_t i = 0; i < num; ++i) {
        existingIds.insert(makeAlias(firstId));
      }
    }
    auto it1 = ids.begin();
    auto it2 = existingIds.begin();
    while (it2 != existingIds.end()) {
      CHECK(it1 != ids.end()) << "ids/existingIds size mismatch";
      idmap[*it1] = *it2;
      ++it1;
      ++it2;
    }
  }

  bt.recordEvent(fmt::format("groups={} maxGroup={}", groupCount, maxGroupSize));
  bt.stop();
  VLOG(1) << bt;
  return idmap;
}

void ZObjDoc::write(json::object& json) const
{
  for (auto id : objs()) {
    json[QString("%1 %2").arg(typeName()).arg(id).toStdString()] = jsonValue(id);
  }
}

QString ZObjDoc::lastOpenedObjPath() const
{
  return ZSystemInfo::instance().lastOpenedObjPath(typeName());
}

void ZObjDoc::setLastOpenedObjPath(const QString& path) const
{
  ZSystemInfo::instance().setLastOpenedObjPath(typeName(), path);
}

QString ZObjDoc::strippedName(const QString& fullFileName)
{
  return QFileInfo(fullFileName).fileName();
}

} // namespace nim
