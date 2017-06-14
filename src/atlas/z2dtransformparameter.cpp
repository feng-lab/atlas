#include "z2dtransformparameter.h"

#include "zwidgetsgroup.h"
#include "zlog.h"
#include "zlogqttypesupport.h"
#include <QWidget>
#include <QGroupBox>
#include <QPushButton>

namespace nim {

Z2DTransformParameter::Z2DTransformParameter(const QString& name, QObject* parent)
  : ZSingleValueParameter<glm::dmat3>(name, glm::dmat3(1), parent)
  , m_scale("Scale", glm::dvec2(1.f), glm::dvec2(std::numeric_limits<double>::lowest()),
            glm::dvec2(std::numeric_limits<double>::max()))
  , m_translation("Translation", glm::dvec2(0), glm::dvec2(std::numeric_limits<double>::lowest()),
                  glm::dvec2(std::numeric_limits<double>::max()))
  , m_rotation("Rotation", 0, std::numeric_limits<double>::lowest(),
               std::numeric_limits<double>::max())
  , m_center("Rotation Center", glm::dvec2(0.f), glm::dvec2(std::numeric_limits<double>::lowest()),
             glm::dvec2(std::numeric_limits<double>::max()))
  , m_receiveWidgetSignal(true)
{
  updateWidget(m_value);

  QStringList names;
  names << "x:" << "y:";
  m_scale.setNameForEachValue(names);
  m_translation.setNameForEachValue(names);
  m_center.setNameForEachValue(names);

  m_scale.setSingleStep(.1);
  m_scale.setDecimal(6);
  m_scale.setStyle("SPINBOX");
  connect(&m_scale, &ZDVec2Parameter::valueChanged, this, &Z2DTransformParameter::updateMatrix);

  m_translation.setSingleStep(10);
  m_translation.setDecimal(6);
  m_translation.setStyle("SPINBOX");
  connect(&m_translation, &ZDVec2Parameter::valueChanged, this, &Z2DTransformParameter::updateMatrix);

  m_rotation.setSingleStep(1);
  m_rotation.setDecimal(6);
  m_rotation.setStyle("SPINBOX");
  connect(&m_rotation, &ZDoubleParameter::valueChanged, this, &Z2DTransformParameter::updateMatrix);

  m_center.setSingleStep(10);
  m_center.setDecimal(6);
  m_center.setStyle("SPINBOX");
  connect(&m_center, &ZDVec2Parameter::valueChanged, this, &Z2DTransformParameter::updateMatrix);
}

Z2DTransformParameter::Z2DTransformParameter(const QString& name, const glm::dmat3& value, QObject* parent)
  : ZSingleValueParameter<glm::dmat3>(name, value, parent)
  , m_scale("Scale", glm::dvec2(1.f), glm::dvec2(std::numeric_limits<double>::lowest()),
            glm::dvec2(std::numeric_limits<double>::max()))
  , m_translation("Translation", glm::dvec2(0), glm::dvec2(std::numeric_limits<double>::lowest()),
                  glm::dvec2(std::numeric_limits<double>::max()))
  , m_rotation("Rotation", 0, std::numeric_limits<double>::lowest(),
               std::numeric_limits<double>::max())
  , m_center("Rotation Center", glm::dvec2(0.f), glm::dvec2(std::numeric_limits<double>::lowest()),
             glm::dvec2(std::numeric_limits<double>::max()))
  , m_receiveWidgetSignal(true)
{
  updateWidget(m_value);

  QStringList names;
  names << "x:" << "y:";
  m_scale.setNameForEachValue(names);
  m_translation.setNameForEachValue(names);
  m_center.setNameForEachValue(names);

  m_scale.setSingleStep(.1);
  m_scale.setDecimal(6);
  m_scale.setStyle("SPINBOX");
  connect(&m_scale, &ZDVec2Parameter::valueChanged, this, &Z2DTransformParameter::updateMatrix);

  m_translation.setSingleStep(10);
  m_translation.setDecimal(6);
  m_translation.setStyle("SPINBOX");
  connect(&m_translation, &ZDVec2Parameter::valueChanged, this, &Z2DTransformParameter::updateMatrix);

  m_rotation.setSingleStep(1);
  m_rotation.setDecimal(6);
  m_rotation.setStyle("SPINBOX");
  connect(&m_rotation, &ZDoubleParameter::valueChanged, this, &Z2DTransformParameter::updateMatrix);

  m_center.setSingleStep(10);
  m_center.setDecimal(6);
  m_center.setStyle("SPINBOX");
  connect(&m_center, &ZDVec2Parameter::valueChanged, this, &Z2DTransformParameter::updateMatrix);
}

void Z2DTransformParameter::rotate(double ang)
{
  setRotation(rotation() + ang);
}

void Z2DTransformParameter::rotate(double ang, const glm::dvec2& center)
{
  glm::dvec2 scale = m_scale.get();
  glm::dmat3 currValue = m_value;
  if (scale.x == 0 || scale.y == 0) {
    scale.x = std::max(scale.x, 0.000001);
    scale.y = std::max(scale.y, 0.000001);
    glm::dmat3 trans1 = glm::translate(glm::dmat3(1), -m_center.get() * scale);
    glm::dmat3 trans = glm::translate(glm::dmat3(1), m_translation.get() + m_center.get() * scale);
    glm::dmat3 scales = glm::scale(glm::dmat3(1), scale);
    glm::dmat3 rot = glm::rotate(glm::dmat3(1), rotation());
    currValue = trans * rot * trans1 * scales;
  }
  glm::dmat3 trans1 = glm::translate(glm::dmat3(1), -center);
  glm::dmat3 rot2 = glm::rotate(glm::dmat3(1), ang);
  glm::dmat3 trans2 = glm::translate(glm::dmat3(1), center);
  currValue = trans2 * rot2 * trans1 * currValue;
  setRotation(rotation() + ang);
  m_translation.set(currValue[2].xy());
  m_center.set(glm::dvec2(0));
}

void Z2DTransformParameter::flipHorizontally(const QRectF& boundRect)
{
  glm::dmat3 currValue = m_value;
  glm::dmat3 trans = glm::translate(glm::dmat3(1), glm::dvec2(-boundRect.left() - boundRect.right(), 0.));
  glm::dmat3 flip = glm::scale(glm::dmat3(1), glm::dvec2(-1, 1));
  currValue = flip * trans * currValue;
  setTranslation(currValue[2].xy());
  double xs = glm::length(currValue[0].xy());
  double ys = glm::length(currValue[1].xy());
  if (currValue[0][0] < 0) {
    xs = -xs;
  }
  if (currValue[1][1] < 0) {
    ys = -ys;
  }
  setScale(xs, ys);
  setRotation(std::atan(currValue[0][1] / currValue[0][0]));
  m_center.set(glm::dvec2(0));
}

void Z2DTransformParameter::flipVertically(const QRectF& boundRect)
{
  glm::dmat3 currValue = m_value;
  glm::dmat3 trans = glm::translate(glm::dmat3(1), glm::dvec2(0., -boundRect.top() - boundRect.bottom()));
  glm::dmat3 flip = glm::scale(glm::dmat3(1), glm::dvec2(1, -1));
  currValue = flip * trans * currValue;
  setTranslation(currValue[2].xy());
  double xs = glm::length(currValue[0].xy());
  double ys = glm::length(currValue[1].xy());
  if (currValue[0][0] < 0) {
    xs = -xs;
  }
  if (currValue[1][1] < 0) {
    ys = -ys;
  }
  setScale(xs, ys);
  setRotation(std::atan(currValue[0][1] / currValue[0][0]));
  m_center.set(glm::dvec2(0));
}

void Z2DTransformParameter::setValueSameAs(const ZParameter& rhs)
{
  CHECK(this->isSameType(rhs));
  const Z2DTransformParameter& src = static_cast<const Z2DTransformParameter&>(rhs);
  m_scale.set(src.m_scale.get());
  m_translation.set(src.m_translation.get());
  m_rotation.set(src.m_rotation.get());
  m_center.set(src.m_center.get());
}

void Z2DTransformParameter::interpolate(const ZParameter& prev, double progress, ZParameter& dest)
{
  CHECK(this->isSameType(prev) && this->isSameType(dest));
  const Z2DTransformParameter& prevPara = static_cast<const Z2DTransformParameter&>(prev);
  Z2DTransformParameter& desPara = static_cast<Z2DTransformParameter&>(dest);
  desPara.setScale(glm::mix(prevPara.scale(), scale(), progress));
  desPara.setRotation(glm::mix(prevPara.m_rotation.get(), m_rotation.get(), progress));
  //desPara.setRotation(glm::vec4(glm::mix(prevPara.m_rotation.get().x, m_rotation.get().x, progress), prevPara.m_rotation.get().yzw()));
  desPara.setTranslation(glm::mix(prevPara.translation(), translation(), progress));
  //desPara.setRotationCenter(glm::mix(prevPara.m_center.get(), m_center.get(), progress));
  desPara.setRotationCenter(progress >= 1.0 ? m_center.get() : prevPara.m_center.get());
}

void Z2DTransformParameter::updateMatrix()
{
  if (m_receiveWidgetSignal) {
    glm::dmat3 trans1 = glm::translate(glm::dmat3(1), -m_center.get() * m_scale.get());
    glm::dmat3 trans = glm::translate(glm::dmat3(1), m_translation.get() + m_center.get() * m_scale.get());
    glm::dmat3 scale = glm::scale(glm::dmat3(1), m_scale.get());
    glm::dmat3 rot = glm::rotate(glm::dmat3(1), rotation());

    m_value = trans * rot * trans1 * scale;

    emit valueChanged();
  }
}

void Z2DTransformParameter::showTransformMatrix()
{
  LOG(INFO) << "Transform: " << m_value;
  LOG(INFO) << "Inverse Transform: " << glm::affineInverse(m_value);
  LOG(INFO) << "";
}

QWidget* Z2DTransformParameter::actualCreateWidget(QWidget* parent)
{
  ZWidgetsGroup transform("Transform", 1);
  transform.addChild(m_scale, 1);
  transform.addChild(m_rotation, 1);
  transform.addChild(m_translation, 1);
  transform.addChild(m_center, 1);

  QPushButton* pb = new QPushButton("Show Transform Matrix");
  connect(pb, &QPushButton::clicked, this, &Z2DTransformParameter::showTransformMatrix);
  transform.addChild(*pb, 2);

  QLayout* lw = transform.createLayout(false);
  //QWidget *widget = new QWidget();
  //widget->setLayout(lw);
  QGroupBox* groupBox = new QGroupBox("Transform Parameters", parent);
  groupBox->setLayout(lw);

  //widget->setParent(parent);
  //return widget;
  return groupBox;
}

void Z2DTransformParameter::beforeChange(glm::dmat3& value)
{
  updateWidget(value);
}

void Z2DTransformParameter::updateWidget(const glm::dmat3& value)
{
  Q_UNUSED(value)
  m_receiveWidgetSignal = false;
  //  m_eye.set(value.getEye());
  //  m_center.set(value.getCenter());
  //  m_upVector.set(value.getUpVector());
  //  if (value.isPerspectiveProjection())
  //    m_projectionType.select("Perspective");
  //  else
  //    m_projectionType.select("Orthographic");
  //  m_eyeSeparationAngle.set(value.getEyeSeparationAngle());
  //  m_fieldOfView.set(value.getFieldOfView());
  //  m_nearDist.set(value.getNearDist());
  //  m_farDist.set(value.getFarDist());
  //  m_nearDist.setRange(1e-10, value.getFarDist());
  //  m_farDist.setRange(value.getNearDist(), std::numeric_limits<float>::max());
  m_receiveWidgetSignal = true;
}

void Z2DTransformParameter::setSameAs(const ZParameter& rhs)
{
  setValueSameAs(rhs);
  ZParameter::setSameAs(rhs);
}

QJsonValue Z2DTransformParameter::jsonValue() const
{
  QJsonObject obj;
  m_scale.write(obj);
  m_translation.write(obj);
  m_rotation.write(obj);
  m_center.write(obj);
  return obj;
}

void Z2DTransformParameter::readValue(const QJsonValue& jsonValue)
{
  if (jsonValue.isObject()) {
    QJsonObject obj = jsonValue.toObject();
    m_scale.read(obj);
    m_translation.read(obj);
    m_rotation.read(obj);
    m_center.read(obj);
  }
}

} // namespace nim
