#include "z3dscene.h"

#include "z3dgl.h"
#include "z3dnetworkevaluator.h"
#include "zimgcache.h"
#include "zimgregioncache.h"

DEFINE_bool(atlas_clear_image_cache_after_rendering,
            false,
            "Clear image cache after rendering, for test, default is false");

namespace nim {

Z3DScene::Z3DScene(int width, int height, bool stereo, QObject* parent)
  : QGraphicsScene(0, 0, width, height, parent)
  , m_networkEvaluator(nullptr)
  , m_isStereoScene(stereo)
  , m_fakeStereoOnce(false)
{}

void Z3DScene::drawBackground(QPainter* /*painter*/, const QRectF& /*rect*/)
{
  if (!m_networkEvaluator) {
    return;
  }

  // QPainter set glclearcolor to white, we set it back
  glClearColor(0.f, 0.f, 0.f, 0.f);

  m_networkEvaluator->process(m_isStereoScene || m_fakeStereoOnce);
  m_fakeStereoOnce = false;
  glFinish();

  ZImgCache::instance().squeeze();
  ZImgRegionCache::instance().squeeze();
  if (FLAGS_atlas_clear_image_cache_after_rendering) {
    ZImgCache::instance().clear();
    ZImgRegionCache::instance().clear();
  }
  LOG(INFO) << "image cache size: " << ZImgCache::instance().size();
  LOG(INFO) << "image block cache size: " << ZImgRegionCache::instance().size();
}

} // namespace nim
