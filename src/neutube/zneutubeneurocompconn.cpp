#include "zneutubeneurocompconn.h"

#include "zlog.h"

namespace nim::neutube {

void defaultNeurocompConnLegacyLike(NeurocompConnLegacyLike& conn)
{
  conn.mode = NeurocompConnModeLegacyLike::None;
  conn.info[0] = 0;
  conn.info[1] = 0;
  conn.pos = {0.0, 0.0, 0.0};
  conn.ort = {0.0, 0.0, 0.0};
  conn.cost = 0.0;
  conn.pdist = -1.0;
  conn.sdist = -1.0;
}

void neurocompConnTranslateModeLegacyLike(int len2, NeurocompConnLegacyLike& conn)
{
  if (conn.mode != NeurocompConnModeLegacyLike::HookLoop) {
    return;
  }

  if (conn.info[1] <= 0) {
    conn.mode = NeurocompConnModeLegacyLike::Link;
    conn.info[1] = 0;
  } else if (conn.info[1] >= len2 - 1) {
    conn.mode = NeurocompConnModeLegacyLike::Link;
    conn.info[1] = 1;
  }
}

} // namespace nim::neutube
