#include "zobjdoc.h"

#include "zdoc.h"
#include "zbenchtimer.h"
#include "zlog.h"
#include "zfileutils.h"
#include "zchooseobjdialog.h"
#include "zsysteminfo.h"
#include <QMessageBox>
#include <QApplication>
#include <QFileInfo>
#include <algorithm>
#include <bit>
#include <cstdint>
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
  auto allObjs = objs();

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

  for (const auto& group : groups) {
    CHECK(group.value != nullptr);
    const json::value& jv = *group.value;
    const std::vector<size_t>& ids = group.ids;

    // check existing objects that are pointing to jv
    std::set<size_t> existingIds;
    for (auto obj : allObjs) {
      if (isSameObj(jv, jsonValue(obj))) {
        existingIds.insert(obj);
      }
    }

    if (existingIds.empty()) {
      QString errMsg;
      // VLOG(1) << jv;
      size_t id = loadFile(jv, errMsg);
      //VLOG(1) << jv << errMsg;
      if (id == 0) {
        err += QString("%1\n").arg(errMsg);
        continue;
      }
      existingIds.insert(id);
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
