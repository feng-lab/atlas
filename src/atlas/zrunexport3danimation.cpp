#include "zrunexport3danimation.h"

#include "zlog.h"
#include "zdoc.h"
#include "z3drenderingengine.h"
#include <folly/ScopeGuard.h>

DEFINE_bool(run_export_3d_animation, false, "run exporting 3d animation in command line mode");

namespace nim {

int ZRunExport3DAnimation::run()
{
  LOG(INFO) << "Export 3D Animation Start";
  auto guard = folly::makeGuard([]() {
    LOG(INFO) << "Export 3D Animation End";
  });

  ZDoc doc;
  LOG(INFO) << "1";
  Z3DRenderingEngine engine(doc);
  LOG(INFO) << "1";
  engine.init();
  LOG(INFO) << "1";

  return 0;
}

} // namespace nim
