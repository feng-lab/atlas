#include "z3danimation.h"
#include "z3drenderingengine.h"
#include "zcameraparameteranimation.h"
#include "zdoc.h"
#include "zexception.h"
#include "zjson.h"

namespace nim {

Z3DAnimation::Z3DAnimation(ZDoc& doc, QObject* parent)
  : ZAnimation(doc, parent)
{
  m_cameraParameterAnimation = new ZCameraParameterAnimation("Camera", QColor(0, 255, 0), this);
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

void Z3DAnimation::load(const QString& fn, bool showLoadIssuesDialog)
{
  readContent(fn, "Animation3D", showLoadIssuesDialog);
  m_cameraParameterAnimation = static_cast<ZCameraParameterAnimation*>(m_globalParaAnimations[0].get());
  LOG(INFO) << "Finish loading animation";
}

double Z3DAnimation::readDurationFromFile(const QString& fn)
{
  const json::object root = loadJsonObject(fn);
  const auto animationIt = root.find("Animation3D");
  if (animationIt == root.end() || !animationIt->value().is_object()) {
    throw ZException("File is not Animation3D format");
  }

  const json::object& animation = animationIt->value().as_object();
  const auto docIt = animation.find("Doc");
  if (docIt == animation.end() || !docIt->value().is_object()) {
    throw ZException("Animation3D.Doc is not an object");
  }

  const auto durationIt = animation.find("Duration");
  if (durationIt == animation.end()) {
    return kDefaultDuration;
  }
  if (!durationIt->value().is_number()) {
    throw ZException("Animation3D.Duration is not a number");
  }
  return normalizedDuration(durationIt->value().to_number<double>());
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
