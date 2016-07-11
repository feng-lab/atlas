#include "z2danimation.h"

#include "cassert"
#include "zwidgetsgroup.h"
#include "znumericparameter.h"
#include "zparameteranimation.h"
#include "zview.h"
#include "zdoc.h"
#include "zexception.h"
#include <QApplication>
#include <QMessageBox>
#include <QFile>
#include <QFileInfo>
#include "zobjdoc.h"

namespace nim {

Z2DAnimation::Z2DAnimation(ZDoc &doc, QObject *parent)
  : ZAnimation(doc, parent)
{
  m_sliceAnimation = new ZParameterAnimation("Slice", "Int", QColor(0,255,0));
  m_timeAnimation = new ZParameterAnimation("Time", "Int", QColor(0,255,0));
  m_mipAnimation = new ZParameterAnimation("Z Projection", "Bool", QColor(0,255,0));
  m_viewportAnimation = new ZParameterAnimation("Viewport", "DVec4", QColor(0,255,0));
  m_globalParaAnimations.emplace_back(m_viewportAnimation);
  m_globalParaAnimations.emplace_back(m_sliceAnimation);
  m_globalParaAnimations.emplace_back(m_timeAnimation);
  m_globalParaAnimations.emplace_back(m_mipAnimation);
}

void Z2DAnimation::bindView(ZView *v)
{
  if (m_view == v)
    return;

  if (v) {
    connect(v, &ZView::objViewReady, this, &Z2DAnimation::tryLinkAnimationWith);
    m_view = v;
    rebindView();
  } else {
    releaseParameters();
    m_view = nullptr;
  }
}

void Z2DAnimation::load(const QString &fn)
{
  readContent(fn, "Animation2D");
  m_viewportAnimation = m_globalParaAnimations[0].get();
  m_sliceAnimation = m_globalParaAnimations[1].get();
  m_timeAnimation = m_globalParaAnimations[2].get();
  m_mipAnimation = m_globalParaAnimations[3].get();
}

void Z2DAnimation::save(const QString &fn)
{
  writeContent(fn, "Animation2D");
}

void Z2DAnimation::bindGlobalParameters()
{
  m_sliceAnimation->bindParameter(static_cast<ZView*>(m_view)->slicePara());
  m_timeAnimation->bindParameter(static_cast<ZView*>(m_view)->timePara());
  m_mipAnimation->bindParameter(static_cast<ZView*>(m_view)->mipPara());
  m_viewportAnimation->bindParameter(static_cast<ZView*>(m_view)->viewportPara());
}

void Z2DAnimation::addGlobalKey(double time)
{
  // global settings
  ZIntParameter& slicePara = static_cast<ZView*>(m_view)->slicePara();
  ZParameterKey *skey = new ZParameterKey(time, slicePara);
  m_sliceAnimation->addKey(skey);
  ZIntParameter& timePara = static_cast<ZView*>(m_view)->timePara();
  ZParameterKey *tkey = new ZParameterKey(time, timePara);
  m_timeAnimation->addKey(tkey);
  ZBoolParameter& mipPara = static_cast<ZView*>(m_view)->mipPara();
  ZParameterKey *mkey = new ZParameterKey(time, mipPara);
  m_mipAnimation->addKey(mkey);
  ZDVec4Parameter& viewportPara = static_cast<ZView*>(m_view)->viewportPara();
  ZParameterKey *vkey = new ZParameterKey(time, viewportPara);
  m_viewportAnimation->addKey(vkey);
}

} // namespace nim
