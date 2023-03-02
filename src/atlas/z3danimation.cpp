#include "z3danimation.h"
#include "z3drenderingengine.h"
#include "zcameraparameteranimation.h"
#include "zdoc.h"

namespace nim {

Z3DAnimation::Z3DAnimation(ZDoc& doc, QObject* parent)
  : ZAnimation(doc, parent)
{
  m_cameraParameterAnimation = new ZCameraParameterAnimation("Camera", QColor(0, 255, 0));
  m_globalParaAnimations.emplace_back(m_cameraParameterAnimation);
}

void Z3DAnimation::bindView(Z3DRenderingEngine* v)
{
  if (m_engine == v) {
    return;
  }

  if (v) {
    connect(v, &Z3DRenderingEngine::objViewReady, this, &Z3DAnimation::tryLinkAnimationWith);
    m_engine = v;
    rebindView();
  } else {
    releaseParameters();
    m_engine = nullptr;
  }
}

void Z3DAnimation::load(const QString& fn)
{
  readContent(fn, "Animation3D");
  m_cameraParameterAnimation = static_cast<ZCameraParameterAnimation*>(m_globalParaAnimations[0].get());
  LOG(INFO) << "Finish loading animation";
}

void Z3DAnimation::save(const QString& fn)
{
  writeContent(fn, "Animation3D");
}

void Z3DAnimation::bindGlobalParameters()
{
  m_cameraParameterAnimation->bindParameter(static_cast<Z3DRenderingEngine*>(m_engine)->camera());
}

void Z3DAnimation::addGlobalKey(double time)
{
  // camera
  m_cameraParameterAnimation->addKey(
    std::make_unique<ZCameraParameterKey>(time, static_cast<Z3DRenderingEngine*>(m_engine)->camera()));
}

} // namespace nim
