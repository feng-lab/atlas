#include "z2danimation.h"
#include "zdoc.h"
#include "znumericparameter.h"
#include "zoptionparameter.h"
#include "zview.h"
#include <QApplication>

namespace nim {

Z2DAnimation::Z2DAnimation(ZDoc& doc, QObject* parent)
  : ZAnimation(doc, parent)
{
  m_sliceAnimation = new ZParameterAnimation("Slice", "Int", QColor(0, 255, 0));
  m_globalParaAnimations.emplace_back(m_sliceAnimation);
  m_timeAnimation = new ZParameterAnimation("Time", "Int", QColor(0, 255, 0));
  m_globalParaAnimations.emplace_back(m_timeAnimation);
  m_viewStyleAnimation = new ZParameterAnimation("View Style", "StringIntOption", QColor(0, 255, 0));
  m_globalParaAnimations.emplace_back(m_viewStyleAnimation);
  m_viewportAnimation = new ZParameterAnimation("Viewport", "DVec4", QColor(0, 255, 0));
  m_globalParaAnimations.emplace_back(m_viewportAnimation);
}

void Z2DAnimation::bindView(ZView* v)
{
  if (m_view == v) {
    return;
  }

  if (v) {
    connect(v, &ZView::objViewReady, this, &Z2DAnimation::tryLinkAnimationWith);
    m_view = v;
    rebindView();
  } else {
    releaseParameters();
    m_view = nullptr;
  }
}

void Z2DAnimation::load(const QString& fn)
{
  readContent(fn, "Animation2D");
  m_viewportAnimation = m_globalParaAnimations[0].get();
  m_sliceAnimation = m_globalParaAnimations[1].get();
  m_timeAnimation = m_globalParaAnimations[2].get();
  m_viewStyleAnimation = m_globalParaAnimations[3].get();
}

void Z2DAnimation::save(const QString& fn)
{
  writeContent(fn, "Animation2D");
}

void Z2DAnimation::bindGlobalParameters()
{
  m_sliceAnimation->bindParameter(static_cast<ZView*>(m_view)->slicePara());
  m_timeAnimation->bindParameter(static_cast<ZView*>(m_view)->timePara());
  m_viewStyleAnimation->bindParameter(static_cast<ZView*>(m_view)->viewStylePara());
  m_viewportAnimation->bindParameter(static_cast<ZView*>(m_view)->viewportPara());
}

void Z2DAnimation::addGlobalKey(double time)
{
  // global settings
  m_sliceAnimation->addKey(std::make_unique<ZParameterKey>(time, static_cast<ZView*>(m_view)->slicePara()));
  m_timeAnimation->addKey(std::make_unique<ZParameterKey>(time, static_cast<ZView*>(m_view)->timePara()));
  m_viewStyleAnimation->addKey(std::make_unique<ZParameterKey>(time, static_cast<ZView*>(m_view)->viewStylePara()));
  m_viewportAnimation->addKey(std::make_unique<ZParameterKey>(time, static_cast<ZView*>(m_view)->viewportPara()));
}

} // namespace nim
