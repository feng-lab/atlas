#include "z3danimationfilter.h"

#include "z3dcameraparameter.h"
#include "z3dgpuinfo.h"
#include "zcameraparameteranimation.h"
#include "zeventlistenerparameter.h"
#include <QFileInfo>
#include <boost/math/constants/constants.hpp>

namespace nim {

Z3DAnimationFilter::Z3DAnimationFilter(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DGeometryFilter(globalParas, parent)
  , m_lineRenderer(m_rendererBase)
  , m_arrowRenderer(m_rendererBase)
  , m_triangleListRenderer(m_rendererBase)
  , m_dataIsInvalid(false)
  , m_animation(nullptr)
  , m_lineWidth("Line Width", 2, 1, 100)
  , m_colorMode("Color Mode")
  , m_color("Color", glm::vec4(1, 1, 0, 1))
  , m_colorMap("Color Map", 0.0, 1.0, QColor(255, 0, 0), QColor(0, 0, 255))
  , m_timeInterval("Time Interval", .01, .01, 100)
  , m_cameraSize("Camera Size")
  , m_showCameraDirection("Show Interpolated Camera Direction", true)
  , m_cameraDirectionSize("Camera Direction Arrow Size")
  , m_upDirectionColor("Up Direction Color", glm::vec4(0, 1, 0, 1))
  , m_viewDirectionColor("View Direction Color", glm::vec4(0, 0, 1, 1))
  , m_cameraDirectionTimeInterval("Camera Direction Time Interval", .5, .1, 100)
{
  setTransformEnabled(false);
  // m_visible.set(false);

  m_colorMode.addOptions("Single Color", "Colormap Time");
  m_colorMode.select("Colormap Time");
  m_color.setStyle("COLOR");

  m_timeInterval.setDecimal(2);
  m_timeInterval.setSingleStep(.01);
  connect(&m_lineWidth, &ZIntParameter::valueChanged, this, &Z3DAnimationFilter::updateLineWidth);
  connect(&m_colorMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DAnimationFilter::prepareColor);
  connect(&m_colorMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DAnimationFilter::adjustWidgets);
  connect(&m_color, &ZVec4Parameter::valueChanged, this, &Z3DAnimationFilter::prepareColor);
  connect(&m_colorMap, &ZColorMapParameter::valueChanged, this, &Z3DAnimationFilter::prepareColor);
  connect(&m_timeInterval, &ZDoubleParameter::valueChanged, this, &Z3DAnimationFilter::updateData);

  m_cameraSize.setStyle("SPINBOX");
  m_cameraDirectionSize.setStyle("SPINBOX");
  m_upDirectionColor.setStyle("COLOR");
  m_viewDirectionColor.setStyle("COLOR");
  m_cameraDirectionTimeInterval.setDecimal(1);
  m_cameraDirectionTimeInterval.setSingleStep(.1);
  connect(&m_cameraSize, &ZFloatParameter::valueChanged, this, &Z3DAnimationFilter::updateData);
  connect(&m_showCameraDirection, &ZBoolParameter::valueChanged, this, &Z3DAnimationFilter::adjustWidgets);
  connect(&m_cameraDirectionSize, &ZFloatParameter::valueChanged, this, &Z3DAnimationFilter::updateData);
  connect(&m_upDirectionColor, &ZVec4Parameter::valueChanged, this, &Z3DAnimationFilter::prepareColor);
  connect(&m_viewDirectionColor, &ZVec4Parameter::valueChanged, this, &Z3DAnimationFilter::prepareColor);
  connect(&m_cameraDirectionTimeInterval, &ZDoubleParameter::valueChanged, this, &Z3DAnimationFilter::updateData);

  addParameter(m_lineWidth);
  addParameter(m_colorMode);
  addParameter(m_color);
  addParameter(m_colorMap);
  addParameter(m_timeInterval);
  addParameter(m_cameraSize);
  addParameter(m_showCameraDirection);
  addParameter(m_cameraDirectionSize);
  addParameter(m_upDirectionColor);
  addParameter(m_viewDirectionColor);
  addParameter(m_cameraDirectionTimeInterval);

  m_triangles.setType(ZMesh::Type::TRIANGLES);
  m_trianglesWrapper.push_back(&m_triangles);

  m_lineRenderer.setLineWidth(m_lineWidth.get());
}

double Z3DAnimationFilter::process(Z3DEye)
{
  syncRendererState();

  if (m_dataIsInvalid) {
    prepareData();
  }
  return 1.0;
}

void Z3DAnimationFilter::setData(Z3DAnimation* animation)
{
  if (m_animation) {
    m_animation->disconnect(this);
  }
  m_animation = animation;
  if (m_animation) {
    connect(cameraParaAnimation(),
            &ZCameraParameterAnimation::keysChanged,
            this,
            &Z3DAnimationFilter::updateData,
            Qt::UniqueConnection);
    connect(cameraParaAnimation(),
            &ZCameraParameterAnimation::keyChanged,
            this,
            &Z3DAnimationFilter::updateData,
            Qt::UniqueConnection);
    connect(cameraParaAnimation(),
            &ZCameraParameterAnimation::interpolationMethodChanged,
            this,
            &Z3DAnimationFilter::updateData,
            Qt::UniqueConnection);
  }
  updateData();
}

bool Z3DAnimationFilter::isReady(Z3DEye eye) const
{
  return Z3DGeometryFilter::isReady(eye) && m_visible.get() && m_animation;
}

std::shared_ptr<ZWidgetsGroup> Z3DAnimationFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Animation3D", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(cameraParaAnimation()->interpolationMethodPara(), 1);
    m_widgetsGroup->addChild(m_lineWidth, 1);
    m_widgetsGroup->addChild(m_colorMode, 1);
    m_widgetsGroup->addChild(m_color, 1);
    m_widgetsGroup->addChild(m_colorMap, 1);
    m_widgetsGroup->addChild(m_timeInterval, 1);
    m_widgetsGroup->addChild(m_cameraSize, 1);
    m_widgetsGroup->addChild(m_showCameraDirection, 2);
    m_widgetsGroup->addChild(m_cameraDirectionSize, 2);
    m_widgetsGroup->addChild(m_upDirectionColor, 2);
    m_widgetsGroup->addChild(m_viewDirectionColor, 2);
    m_widgetsGroup->addChild(m_cameraDirectionTimeInterval, 2);
  }
  return m_widgetsGroup;
}

void Z3DAnimationFilter::renderOpaque(Z3DEye eye)
{
  if (m_showCameraDirection.get()) {
    m_rendererBase.render(eye, m_lineRenderer, m_arrowRenderer, m_triangleListRenderer);
  } else {
    m_rendererBase.render(eye, m_lineRenderer, m_triangleListRenderer);
  }
}

void Z3DAnimationFilter::renderTransparent(Z3DEye eye)
{
  if (m_showCameraDirection.get()) {
    m_rendererBase.render(eye, m_lineRenderer, m_arrowRenderer, m_triangleListRenderer);
  } else {
    m_rendererBase.render(eye, m_lineRenderer, m_triangleListRenderer);
  }
}

void Z3DAnimationFilter::renderPicking(Z3DEye /*eye*/)
{
  if (!m_pickingObjectsRegistered) {
    registerPickingObjects();
  }

  // m_rendererBase.activateRenderer(m_lineRenderer);
  // m_rendererBase.renderPicking(eye);
}

void Z3DAnimationFilter::prepareData()
{
  if (!m_dataIsInvalid) {
    return;
  }

  deregisterPickingObjects();

  m_lineRenderer.setData(m_lines);
  m_arrowRenderer.setArrowData(&m_tailPosAndTailRadius, &m_headPosAndHeadRadius, .2);
  m_triangleListRenderer.setData(&m_trianglesWrapper);

  prepareColor();
  adjustWidgets();
  m_dataIsInvalid = false;
}

void Z3DAnimationFilter::registerPickingObjects() {}

void Z3DAnimationFilter::deregisterPickingObjects() {}

void Z3DAnimationFilter::updateNotTransformedBoundBoxImpl()
{
  m_notTransformedBoundBox.reset();
  for (auto& line : m_lines) {
    m_notTransformedBoundBox.expand(glm::dvec3(line));
  }
  if (!m_lines.empty()) {
    double cameraSize = std::max(m_cameraDirectionSize.get(), m_cameraSize.get());
    m_notTransformedBoundBox.expand(cameraSize);
  }
}

void Z3DAnimationFilter::prepareColor()
{
  m_lineColors.clear();
  m_arrowColors.clear();

  if (m_colorMode.isSelected("Single Color")) {
    for (size_t i = 0; i < m_times.size(); ++i) {
      m_lineColors.push_back(m_color.get());
    }
  } else if (m_colorMode.isSelected("Colormap Time")) {
    if (m_times.size() == 1) {
      m_lineColors.push_back(m_colorMap.get().mappedFColor(0));
    } else if (m_times.size() > 1) {
      double startTime = m_times[0];
      double endTime = m_times.back();
      for (auto time : m_times) {
        m_lineColors.push_back(m_colorMap.get().mappedFColor((time - startTime) / (endTime - startTime)));
      }
    }
  }

  for (size_t i = 0; i < m_cameraDirectionTimes.size(); ++i) {
    m_arrowColors.push_back(m_viewDirectionColor.get());
    m_arrowColors.push_back(m_upDirectionColor.get());
  }

  if (m_animation) {
    const auto& keys = cameraParaAnimation()->keys();
    std::vector<glm::vec4> colors;
    if (m_colorMode.isSelected("Single Color")) {
      for (size_t i = 0; i < keys.size(); ++i) {
        colors.push_back(m_color.get());
        colors.push_back(m_color.get());
        colors.push_back(m_color.get());
        for (auto j = 0; j < 16; ++j) {
          m_lineColors.push_back(m_color.get());
        }
      }
    } else if (m_colorMode.isSelected("Colormap Time")) {
      if (keys.size() == 1) {
        glm::vec4 color = m_colorMap.get().mappedFColor(0);
        colors.push_back(color);
        colors.push_back(color);
        colors.push_back(color);
        for (auto j = 0; j < 16; ++j) {
          m_lineColors.push_back(color);
        }
      } else if (keys.size() > 1) {
        double startTime = keys[0]->time();
        double endTime = std::max(startTime + 0.01, keys.back()->time());
        for (const auto& key : keys) {
          glm::vec4 color = m_colorMap.get().mappedFColor((key->time() - startTime) / (endTime - startTime));
          colors.push_back(color);
          colors.push_back(color);
          colors.push_back(color);
          for (auto j = 0; j < 16; ++j) {
            m_lineColors.push_back(color);
          }
        }
      }
    }
    m_triangles.setColors(colors);
  }

  m_lineRenderer.setDataColors(std::move(m_lineColors));
  m_arrowRenderer.setArrowColors(&m_arrowColors);
}

void Z3DAnimationFilter::setClipPlanes() {}

void Z3DAnimationFilter::updateData()
{
  if (!m_mutex.try_lock()) {
    return;
  }

  m_dataIsInvalid = true;
  invalidateResult();

  m_lines.clear();
  m_times.clear();
  m_tailPosAndTailRadius.clear();
  m_headPosAndHeadRadius.clear();
  m_cameraDirectionTimes.clear();

  m_timeInterval.setRange(0.01, m_animation->duration() / 2.);
  m_cameraDirectionTimeInterval.setRange(.01, m_animation->duration() / 2.);

  if (m_animation) {
    const auto& keys = cameraParaAnimation()->keys();
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
      double currentKeyTime = keys[i]->time();
      double nextKeyTime = keys[i + 1]->time();
      for (double t = currentKeyTime; t < nextKeyTime; t += m_timeInterval.get()) {
        if (m_times.empty() || t > m_times.back() + 0.0001) { // make sure no overlap
          m_times.push_back(t);
        }
      }
      for (double t = currentKeyTime + m_cameraDirectionTimeInterval.get(); t < nextKeyTime;
           t += m_cameraDirectionTimeInterval.get()) {
        m_cameraDirectionTimes.push_back(t);
      }
    }
    if (!keys.empty() && (m_times.empty() || keys.back()->time() > m_times.back() + 0.0001)) {
      m_times.push_back(keys.back()->time());
    }

    if (m_times.size() <= 1) {
      m_times.clear();
    } else {
      std::vector<double> times;
      times.push_back(m_times[0]);
      size_t i = 1;
      for (; i < m_times.size() - 1; ++i) {
        times.push_back(m_times[i]);
        times.push_back(m_times[i]);
      }
      times.push_back(m_times[i]);
      m_times.swap(times);
    }

    Z3DCameraParameter para("Tmp");
    for (auto time : m_times) {
      cameraParaAnimation()->updateParaToTime(time, &para);
      m_lines.push_back(para.get().eye());
    }

    m_cameraDirectionSize.set(100);
    updateBoundBox();
    auto bbsz = m_axisAlignedBoundBox.size();
    double arrowSize = std::max(bbsz.z, std::max(bbsz.x, bbsz.y)) / 50.;
    if (arrowSize < 0 || arrowSize > std::numeric_limits<float>::max()) {
      arrowSize = 10;
    }
    m_cameraDirectionSize.set(arrowSize);
    m_cameraSize.set(arrowSize * 2.5);
    for (auto time : m_cameraDirectionTimes) {
      cameraParaAnimation()->updateParaToTime(time, &para);
      m_tailPosAndTailRadius.emplace_back(para.get().eye(), m_cameraDirectionSize.get() / 20.f);
      m_headPosAndHeadRadius.emplace_back(para.get().eye() + m_cameraDirectionSize.get() * para.get().viewVector(),
                                          m_cameraDirectionSize.get() / 10.f);
      m_tailPosAndTailRadius.emplace_back(para.get().eye(), m_cameraDirectionSize.get() / 20.f);
      m_headPosAndHeadRadius.emplace_back(para.get().eye() + m_cameraDirectionSize.get() * para.get().upVector(),
                                          m_cameraDirectionSize.get() / 10.f);
    }

    m_triangles.clear();
    using namespace boost::math::double_constants;
    float halfHeight = std::tan(pi / 8.) * m_cameraSize.get();
    float halfWidth = std::tan(sixth_pi) * m_cameraSize.get();
    float triangleWidth = halfWidth / 2.f;
    std::vector<glm::vec3> vertices;
    std::vector<glm::vec3> normals;
    for (const auto& key : keys) {
      cameraParaAnimation()->updateParaToTime(key->time(), &para);
      glm::vec3 pt5 = para.get().eye() + m_cameraSize.get() * para.get().viewVector();
      glm::vec3 pt1 = pt5 + halfHeight * para.get().upVector() + halfWidth * para.get().strafeVector();
      glm::vec3 pt2 = pt5 - halfHeight * para.get().upVector() + halfWidth * para.get().strafeVector();
      glm::vec3 pt3 = pt5 - halfHeight * para.get().upVector() - halfWidth * para.get().strafeVector();
      glm::vec3 pt4 = pt5 + halfHeight * para.get().upVector() - halfWidth * para.get().strafeVector();
      glm::vec3 pt0 = para.get().eye();
      m_lines.push_back(pt0);
      m_lines.push_back(pt1);
      m_lines.push_back(pt0);
      m_lines.push_back(pt2);
      m_lines.push_back(pt0);
      m_lines.push_back(pt3);
      m_lines.push_back(pt0);
      m_lines.push_back(pt4);
      m_lines.push_back(pt1);
      m_lines.push_back(pt2);
      m_lines.push_back(pt2);
      m_lines.push_back(pt3);
      m_lines.push_back(pt3);
      m_lines.push_back(pt4);
      m_lines.push_back(pt4);
      m_lines.push_back(pt1);
      vertices.push_back(pt5 + (1.2f * halfHeight + triangleWidth * 1.73f) * para.get().upVector());
      vertices.push_back(pt5 + (1.2f * halfHeight) * para.get().upVector() + triangleWidth * para.get().strafeVector());
      vertices.push_back(pt5 + (1.2f * halfHeight) * para.get().upVector() - triangleWidth * para.get().strafeVector());
      normals.push_back(para.get().viewVector());
      normals.push_back(para.get().viewVector());
      normals.push_back(para.get().viewVector());
    }
    m_triangles.setVertices(vertices);
    m_triangles.setNormals(normals);
  }
  updateBoundBox();
  m_mutex.unlock();
}

void Z3DAnimationFilter::adjustWidgets()
{
  m_color.setVisible(m_colorMode.isSelected("Single Color"));
  m_colorMap.setVisible(m_colorMode.isSelected("Colormap Time"));

  m_cameraDirectionSize.setVisible(m_showCameraDirection.get());
  m_upDirectionColor.setVisible(m_showCameraDirection.get());
  m_viewDirectionColor.setVisible(m_showCameraDirection.get());
  m_cameraDirectionTimeInterval.setVisible(m_showCameraDirection.get());
}

void Z3DAnimationFilter::updateLineWidth()
{
  m_lineRenderer.setLineWidth(m_lineWidth.get());
}

} // namespace nim
