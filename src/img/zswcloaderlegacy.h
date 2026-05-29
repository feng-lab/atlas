#pragma once

#include "zswc.h"

#include <QString>
#include <string>

namespace nim {

// Loads an SWC file into `out` matching legacy neuTube `Swc_Tree_Parse_String` child/root ordering:
// - Nodes are connected by scanning IDs in increasing order.
// - Each node is inserted as the *first* child of its parent (prepend), which yields descending-id sibling order.
// - Missing/invalid parents are treated as -1 (root).
//
// This ordering is important for strict A/B parity when legacy code relies on:
// - depth-first traversal order, and
// - `firstChild()` selection (e.g., connect-branch heuristics).
//
// Returns false on failure and optionally populates `error`.
//
// `error` is optional; pass a non-null pointer to receive a human-readable error message.
[[nodiscard]] bool loadSwcLegacyOrder(const QString& path, ZSwc& out, /*nullable*/ std::string* error = nullptr);

} // namespace nim
