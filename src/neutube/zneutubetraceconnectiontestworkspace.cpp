#include "zneutubetraceconnectiontestworkspace.h"

#include "zlog.h"

namespace nim::neutube {

void defaultConnectionTestWorkspaceLegacyLike(ConnectionTestWorkspaceLegacyLike* ctw)
{
  CHECK(ctw != nullptr);

  ctw->hookSpot = -1;
  ctw->dist = NeurosegDefaultHLegacyLike * 10.0;
  ctw->cos1 = 0.0;
  ctw->cos2 = 0.0;
  ctw->distThre = NeurosegDefaultHLegacyLike;
  ctw->goodDist = false;
  ctw->resolution = {1.0, 1.0, 1.0};
  ctw->unit = 'p';
  ctw->bigEuc = 15.0;
  ctw->bigPlanar = 10.0;
  ctw->spTest = true;
  ctw->interpolate = true;
  ctw->crossoverTest = false;
  ctw->mask = nullptr;
}

} // namespace nim::neutube
