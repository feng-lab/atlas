#include "zscenejsonio.h"

#include "zdoc.h"
#include "zjson.h"
#include "zqobjectthreadinvoker.h"
#include "zview.h"
#include "z3drenderingengine.h"

#include <QThread>

namespace nim {

bool ZSceneJsonIO::saveToPath(ZDoc* doc, ZView* view, Z3DRenderingEngine* engineOrNull, const QString& path, QString& error)
{
  try {
    if (!doc) {
      error = "doc not ready";
      return false;
    }
    if (!view) {
      error = "view not ready";
      return false;
    }
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
      error = "path is empty";
      return false;
    }

    if (engineOrNull) {
      QThread* t = engineOrNull->thread();
      if (!t || !t->isRunning() || t->isFinished()) {
        // If a 3D window exists, we must include 3D state; fail fast instead
        // of writing a partial scene file.
        error = "3D engine not ready";
        return false;
      }
    }

    json::object sceneObj;
    sceneObj["Version"] = 1.0;

    json::object docObj;
    doc->write(docObj, true);
    sceneObj["Doc"] = docObj;

    const auto objs = doc->objs();
    for (auto id : objs) {
      json::object jObj;

      json::object view2DObj;
      view->write(id, view2DObj);
      jObj["View2D"] = view2DObj;

      if (engineOrNull) {
        auto inv = invokeOnObjectThreadWait(
          engineOrNull,
          [engineOrNull, id]() {
            json::object view3DObj;
            engineOrNull->write(id, view3DObj);
            return view3DObj;
          },
          "save_scene:view3d");
        if (!inv.ok) {
          error = QString::fromStdString(inv.error);
          return false;
        }
        jObj["View3D"] = inv.value;
      }

      sceneObj[QString("%1").arg(id).toStdString()] = jObj;
    }

    json::object view2DGeneralObj;
    view->write(view2DGeneralObj);
    sceneObj["View2DGeneral"] = view2DGeneralObj;

    if (engineOrNull) {
      auto inv = invokeOnObjectThreadWait(
        engineOrNull,
        [engineOrNull]() {
          json::object view3DGeneralObj;
          engineOrNull->write(view3DGeneralObj);
          return view3DGeneralObj;
        },
        "save_scene:view3d_general");
      if (!inv.ok) {
        error = QString::fromStdString(inv.error);
        return false;
      }
      sceneObj["View3DGeneral"] = inv.value;
    }

    json::object saveObj;
    saveObj["Scene"] = sceneObj;

    saveJsonObject(saveObj, trimmed);
    return true;
  }
  catch (const std::exception& e) {
    error = e.what();
    return false;
  }
  catch (...) {
    error = "unknown exception";
    return false;
  }
}

} // namespace nim
