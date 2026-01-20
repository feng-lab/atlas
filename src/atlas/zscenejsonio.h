#pragma once

#include <QString>

namespace nim {

class ZDoc;
class ZView;
class Z3DRenderingEngine;

// Centralized scene (.scene) JSON read/write helpers.
//
// This exists so:
// - UI menu actions and RPC calls share the exact same serialization logic.
// - Cross-thread engine snapshots fail cleanly instead of deadlocking.
class ZSceneJsonIO
{
public:
  [[nodiscard]] static bool
  saveToPath(ZDoc* doc, ZView* view, Z3DRenderingEngine* engineOrNull, const QString& path, QString& error);
};

} // namespace nim
