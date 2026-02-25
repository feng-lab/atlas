#include "zneutubeswcloaderlegacy.h"

#include "zlog.h"
#include "zstringutils.h"

#include <absl/strings/ascii.h>
#include <absl/strings/str_split.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nim::neutube {

namespace {

[[nodiscard]] bool parseSwcLineLegacyCompatible(std::string_view line, SwcNode* out)
{
  if (out == nullptr) {
    return false;
  }

  const std::string_view clean = absl::StripAsciiWhitespace(removeComment(line));
  if (clean.empty()) {
    return false;
  }

  const std::vector<std::string_view> fields =
    absl::StrSplit(clean, absl::ByAnyChar(spaces_literal), absl::SkipEmpty());
  if (fields.size() < 7) {
    // Legacy parser ignores malformed lines rather than failing hard.
    return false;
  }

  SwcNode node;
  stringToValue(fields[0], node.id);
  stringToValue(fields[1], node.type);
  stringToValue(fields[2], node.x);
  stringToValue(fields[3], node.y);
  stringToValue(fields[4], node.z);
  stringToValue(fields[5], node.radius);
  stringToValue(fields[6], node.parentID);

  if (fields.size() >= 8) {
    stringToValue(fields[7], node.label);
  }
  if (fields.size() >= 9) {
    stringToValue(fields[8], node.feature);
  }
  if (fields.size() >= 10) {
    stringToValue(fields[9], node.weight);
  }

  *out = node;
  return true;
}

} // namespace

bool loadSwcLegacyOrder(const std::string& path, ZSwc& out, std::string* error)
{
  try {
    out.clear();

    std::ifstream file(path, std::ios_base::in);
    if (!file.is_open()) {
      throw std::runtime_error("Failed to open SWC file '" + path + "': " + std::strerror(errno));
    }

    int64_t maxId = -1;
    std::vector<SwcNode> parsedNodes;
    parsedNodes.reserve(1024);

    std::string line;
    while (std::getline(file, line)) {
      SwcNode node;
      if (!parseSwcLineLegacyCompatible(line, &node)) {
        continue;
      }

      if (node.id < 0) {
        // Legacy `Swc_Tree_Parse_String` cannot represent negative IDs (it builds an array indexed by id+1).
        // Ignore them for parity with the legacy parser's implicit behavior.
        continue;
      }

      maxId = std::max(maxId, node.id);
      parsedNodes.push_back(node);
    }

    if (file.bad()) {
      throw std::runtime_error("Error while reading SWC file '" + path + "': " + std::strerror(errno));
    }

    if (parsedNodes.empty()) {
      return true;
    }

    if (maxId < 0 || maxId > static_cast<int64_t>(std::numeric_limits<int>::max())) {
      throw std::runtime_error("Invalid SWC max id: " + std::to_string(maxId));
    }

    const size_t mapSize = static_cast<size_t>(maxId) + 2;
    std::vector<std::optional<SwcNode>> byId(mapSize);
    for (const auto& node : parsedNodes) {
      const size_t idx = static_cast<size_t>(node.id) + 1;
      CHECK(idx < byId.size());
      byId[idx] = node;
    }

    std::vector<ZSwc::SwcTreeNode> itMap(mapSize, out.end());

    // Create all nodes as roots first (in descending ID order), then attach children
    // using prepend semantics to match legacy `first_child` linkage behavior.
    for (int64_t id = maxId; id >= 0; --id) {
      const size_t idx = static_cast<size_t>(id) + 1;
      if (!byId[idx].has_value()) {
        continue;
      }
      itMap[idx] = out.appendRoot(*byId[idx]);
    }

    auto isParentValid = [&](int64_t nodeId, int64_t parentId) -> bool {
      if (parentId < 0) {
        return parentId == -1;
      }
      if (parentId == nodeId) {
        return false;
      }
      if (parentId > maxId) {
        return false;
      }
      const size_t pidx = static_cast<size_t>(parentId) + 1;
      return (pidx < byId.size() && byId[pidx].has_value());
    };

    for (int64_t id = 0; id <= maxId; ++id) {
      const size_t idx = static_cast<size_t>(id) + 1;
      if (!byId[idx].has_value()) {
        continue;
      }

      auto child = itMap[idx];
      CHECK(!ZSwc::isNull(child));

      int64_t parentId = byId[idx]->parentID;
      if (!isParentValid(id, parentId)) {
        child->parentID = -1;
        continue;
      }
      if (parentId == -1) {
        child->parentID = -1;
        continue;
      }

      const size_t pidx = static_cast<size_t>(parentId) + 1;
      auto parent = itMap[pidx];
      CHECK(!ZSwc::isNull(parent));

      out.prependChild(parent, child);
    }

    return true;
  }
  catch (const std::exception& e) {
    if (error != nullptr) {
      *error = e.what();
    } else {
      LOG(ERROR) << "SWC legacy-order load failed for " << path << ": " << e.what();
    }
    return false;
  }
}

} // namespace nim::neutube
