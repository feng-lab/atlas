#include "z3danimation.h"

#include "cassert"
#include "zwidgetsgroup.h"
#include "zcameraparameteranimation.h"
#include "znumericparameter.h"
#include "z3dview.h"
#include "zdoc.h"
#include "zexception.h"
#include <QApplication>
#include <QMessageBox>
#include "zobjdoc.h"

namespace nim {

Z3DAnimation::Z3DAnimation(ZDoc &doc, QObject *parent)
  : ZAnimation(doc, parent)
{
  m_cameraParameterAnimation = new ZCameraParameterAnimation("Camera", QColor(0,255,0), this);
  m_globalParameters.push_back(m_cameraParameterAnimation);
}

Z3DAnimation::~Z3DAnimation()
{
}

void Z3DAnimation::bindView(Z3DView *v)
{
  if (m_view == v)
    return;

  if (v) {
    connect(v, SIGNAL(objViewReady(size_t)), this, SLOT(tryLinkAnimationWith(size_t)));
    m_view = v;
    rebindView();
  } else {
    releaseParameters();
    m_view = nullptr;
  }
}

void Z3DAnimation::load(const QString &fn)
{
  readContent(fn, "Animation3D");
  m_cameraParameterAnimation = static_cast<ZCameraParameterAnimation*>(m_globalParameters[0]);
  LINFO() << "Finish loading animation";
}

void Z3DAnimation::save(const QString &fn)
{
  writeContent(fn, "Animation3D");
}

void Z3DAnimation::bindGlobalParameters()
{
  m_cameraParameterAnimation->bindParameter(static_cast<Z3DView*>(m_view)->camera());
}

void Z3DAnimation::addGlobalKey(double time)
{
  // camera
  Z3DCameraParameter& camera = static_cast<Z3DView*>(m_view)->camera();
  ZCameraParameterKey *ckey = new ZCameraParameterKey(time, camera);
  m_cameraParameterAnimation->addKey(ckey);
}

} // namespace nim
