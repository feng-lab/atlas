#include "z3dtransformparameter.h"
#include <QWidget>
#include <QGroupBox>
#include <QPushButton>
#include "zwidgetsgroup.h"
#include "zlog.h"

namespace {

#if 0   //not used for now
/**
 * @brief Determine whether this matrix represents an affine transform or not.
 * @return true if this matrix is an affine transform, false if not.
 */
bool isAffine(const glm::mat4 &m) {
  // First make sure the bottom row meets the condition that it is (0, 0, 0, 1)
  if (m[0][3] != 0.f || m[1][3] != 0.f || m[2][3] != 0.f || m[3][3] != 1.f) {
    return false;
  }

  // Get the inverse of this matrix:
  // Make sure the matrix is invertible to begin with...
  if (std::abs(glm::determinant(m)) <= 1e-4) {
    return false;
  }

  // Calculate the inverse and seperate the inverse translation component
  // and the top 3x3 part of the inverse matrix
  glm::mat4 inv4x4Matrix = glm::inverse(m);
  glm::vec3 inv4x4Translation(inv4x4Matrix[3].xyz());
  glm::mat3 inv4x4Top3x3(inv4x4Matrix);

  // Grab just the top 3x3 matrix
  glm::mat3 top3x3Matrix(m);
  glm::mat3 invTop3x3Matrix = glm::inverse(top3x3Matrix);
  glm::vec3 inv3x3Translation = -(invTop3x3Matrix * m[3].xyz());

  // Make sure we adhere to the conditions of a 4x4 invertible affine transform matrix
  if (inv4x4Top3x3 != invTop3x3Matrix) {
    return false;
  }
  if (inv4x4Translation != inv3x3Translation) {
    return false;
  }

  return true;
}

/**
 * @brief Decomposes the given matrix 'm' into its translation, rotation and scale components.
 * @param m The matrix to decompose.
 * @param translation [in,out] The resulting translation component of m.
 * @param rotation [in,out] The resulting rotation component of m.
 * @param scale [in,out] The resulting scale component of m.
 */
void decompose(const glm::mat4& m, glm::vec3& translation, glm::vec3& scale, glm::quat& rot)
{
  // Copy the matrix first - we'll use this to break down each component
  glm::mat4 mCopy(m);

  // Start by extracting the translation (and/or any projection) from the given matrix
  translation = mCopy[3].xyz();
  mCopy[3] = glm::vec4(0,0,0,1);

  // Extract the rotation component - this is done using polar decompostion, where
  // we successively average the matrix with its inverse transpose until there is
  // no/a very small difference between successive averages
  float norm;
  int count = 0;
  glm::mat4 rotation = mCopy;
  do {
    glm::mat4 nextRotation;
    glm::mat4 currInvTranspose = glm::inverse(glm::transpose(rotation));

    nextRotation = (rotation + currInvTranspose) * .5f;

    norm = 0.0;
    for (int i = 0; i < 3; i++) {
      float n = static_cast<float>(
            std::abs(rotation[0][i] - nextRotation[0][i]) +
          std::abs(rotation[1][i] - nextRotation[1][i]) +
          std::abs(rotation[2][i] - nextRotation[2][i]));
      norm = std::max(norm, n);
    }
    rotation = nextRotation;
  } while (count < 100 && norm > 1e-4);
  rot = glm::quat_cast(rotation);

  // The scale is simply the removal of the rotation from the non-translated matrix
  glm::mat4 scaleMatrix = glm::inverse(rotation) * mCopy;
  scale = glm::vec3(scaleMatrix[0][0],
      scaleMatrix[1][1],
      scaleMatrix[2][2]);

  // Calculate the normalized rotation matrix and take its determinant to determine whether
  // it had a negative scale or not...
  glm::vec3 row1(mCopy[0][0], mCopy[1][0], mCopy[2][0]);
  glm::vec3 row2(mCopy[0][1], mCopy[1][1], mCopy[2][1]);
  glm::vec3 row3(mCopy[0][2], mCopy[1][2], mCopy[2][2]);
  row1 = glm::normalize(row1);
  row2 = glm::normalize(row2);
  row3 = glm::normalize(row3);
  glm::mat3 nRotation(row1, row2, row3);  // no need to transpose

  // Special consideration: if there's a single negative scale
  // (all other combinations of negative scales will
  // be part of the rotation matrix), the determinant of the
  // normalized rotation matrix will be < 0.
  // If this is the case we apply an arbitrary negative to one
  // of the component of the scale.
  float determinant = glm::determinant(nRotation);
  if (determinant < 0.0) {
    scale.x *= -1;
  }
}
#endif

}

namespace nim {

Z3DTransformParameter::Z3DTransformParameter(const QString &name, QObject *parent)
  : ZSingleValueParameter<glm::mat4>(name, glm::mat4(1.f), parent)
  , m_scale("Scale", glm::vec3(1.f), glm::vec3(std::numeric_limits<float>::lowest()),
            glm::vec3(std::numeric_limits<float>::max()))
  , m_translation("Translation", glm::vec3(0.f), glm::vec3(std::numeric_limits<float>::lowest()),
             glm::vec3(std::numeric_limits<float>::max()))
  , m_rotation("Rotation", glm::vec4(0,0,1,0), glm::vec4(std::numeric_limits<float>::lowest()),
               glm::vec4(std::numeric_limits<float>::max()))
  , m_center("Rotation Center", glm::vec3(0.f), glm::vec3(std::numeric_limits<float>::lowest()),
             glm::vec3(std::numeric_limits<float>::max()))
  , m_receiveWidgetSignal(true)
{
  updateWidget(m_value);

  QStringList names;
  names << "x:" << "y:" << "z:";
  m_scale.setNameForEachValue(names);
  m_translation.setNameForEachValue(names);
  m_center.setNameForEachValue(names);

  m_scale.setSingleStep(.1);
  m_scale.setDecimal(6);
  m_scale.setStyle("SPINBOX");
  connect(&m_scale, &ZVec3Parameter::valueChanged, this, &Z3DTransformParameter::updateMatrix);

  m_translation.setSingleStep(10);
  m_translation.setDecimal(6);
  m_translation.setStyle("SPINBOX");
  connect(&m_translation, &ZVec3Parameter::valueChanged, this, &Z3DTransformParameter::updateMatrix);

  names.clear();
  names << "angle:" << "x:" << "y:" << "z:";
  m_rotation.setNameForEachValue(names);
  m_rotation.setSingleStep(1);
  m_rotation.setDecimal(6);
  m_rotation.setStyle("SPINBOX");
  connect(&m_rotation, &ZVec4Parameter::valueChanged, this, &Z3DTransformParameter::updateMatrix);

  m_center.setSingleStep(10);
  m_center.setDecimal(6);
  m_center.setStyle("SPINBOX");
  connect(&m_center, &ZVec3Parameter::valueChanged, this, &Z3DTransformParameter::updateMatrix);
}

Z3DTransformParameter::Z3DTransformParameter(const QString &name, const glm::mat4 &value, QObject *parent)
  : ZSingleValueParameter<glm::mat4>(name, value, parent)
  , m_scale("Scale", glm::vec3(1.f), glm::vec3(std::numeric_limits<float>::lowest()),
            glm::vec3(std::numeric_limits<float>::max()))
  , m_translation("Translation", glm::vec3(0.f), glm::vec3(std::numeric_limits<float>::lowest()),
             glm::vec3(std::numeric_limits<float>::max()))
  , m_rotation("Rotation", glm::vec4(0,0,1,0), glm::vec4(std::numeric_limits<float>::lowest()),
               glm::vec4(std::numeric_limits<float>::max()))
  , m_center("Rotation Center", glm::vec3(0.f), glm::vec3(std::numeric_limits<float>::lowest()),
             glm::vec3(std::numeric_limits<float>::max()))
  , m_receiveWidgetSignal(true)
{
  updateWidget(m_value);

  QStringList names;
  names << "x:" << "y:" << "z:";
  m_scale.setNameForEachValue(names);
  m_translation.setNameForEachValue(names);
  m_center.setNameForEachValue(names);

  m_scale.setSingleStep(.1);
  m_scale.setDecimal(6);
  m_scale.setStyle("SPINBOX");
  connect(&m_scale, &ZVec3Parameter::valueChanged, this, &Z3DTransformParameter::updateMatrix);

  m_translation.setSingleStep(10);
  m_translation.setDecimal(6);
  m_translation.setStyle("SPINBOX");
  connect(&m_translation, &ZVec3Parameter::valueChanged, this, &Z3DTransformParameter::updateMatrix);

  names.clear();
  names << "angle:" << "x:" << "y:" << "z:";
  m_rotation.setNameForEachValue(names);
  m_rotation.setSingleStep(1);
  m_rotation.setDecimal(6);
  m_rotation.setStyle("SPINBOX");
  connect(&m_rotation, &ZVec4Parameter::valueChanged, this, &Z3DTransformParameter::updateMatrix);

  m_center.setSingleStep(10);
  m_center.setDecimal(6);
  m_center.setStyle("SPINBOX");
  connect(&m_center, &ZVec3Parameter::valueChanged, this, &Z3DTransformParameter::updateMatrix);
}

glm::quat Z3DTransformParameter::rotation() const
{
  if (glm::length(m_rotation.get().yzw()) > 0)
    return glm::angleAxis(glm::radians(m_rotation.get().x), glm::normalize(m_rotation.get().yzw()));
  else
    return glm::quat();
}

void Z3DTransformParameter::rotate(const glm::vec3 &axis, float ang)
{
  glm::quat quat = rotation() * glm::angleAxis(ang, glm::normalize(axis));
  setRotation(glm::normalize(quat));
}

void Z3DTransformParameter::rotate(const glm::vec3 &axis, float ang, const glm::vec3 &center)
{
  glm::vec3 scale = m_scale.get();
  glm::mat4 currValue = m_value;
  if (scale.x == 0 || scale.y == 0 || scale.z == 0) {
    scale.x = std::max(scale.x, 0.000001f);
    scale.y = std::max(scale.y, 0.000001f);
    scale.z = std::max(scale.z, 0.000001f);
    glm::mat4 trans1 = glm::translate(glm::mat4(1.f), -m_center.get() * scale);
    glm::mat4 trans = glm::translate(glm::mat4(1.f), m_translation.get() + m_center.get() * scale);
    glm::mat4 scales = glm::scale(glm::mat4(1.f), scale);
    glm::mat4 rot = glm::mat4_cast(rotation());
    currValue = trans * rot * trans1 * scales;
  }
  glm::mat4 trans1 = glm::translate(glm::mat4(1.f), -center);
  glm::mat4 rotation = glm::mat4_cast(glm::angleAxis(ang, glm::normalize(axis)));
  glm::mat4 trans2 = glm::translate(glm::mat4(1.f), center);
  currValue = trans2 * rotation * trans1 * currValue;
  glm::mat3 rotationMat(currValue);
  rotationMat[0] /= scale.x;
  rotationMat[1] /= scale.y;
  rotationMat[2] /= scale.z;
  glm::quat quat = glm::normalize(glm::quat_cast(rotationMat));
  setRotation(quat);
  m_translation.set(currValue[3].xyz());
  m_center.set(glm::vec3(0,0,0));
}

void Z3DTransformParameter::setValueSameAs(const ZParameter &rhs)
{
  assert(this->isSameType(rhs));
  const Z3DTransformParameter& src = static_cast<const Z3DTransformParameter&>(rhs);
  m_scale.set(src.m_scale.get());
  m_translation.set(src.m_translation.get());
  m_rotation.set(src.m_rotation.get());
  m_center.set(src.m_center.get());
}

void Z3DTransformParameter::interpolate(const ZParameter &prev, double progress, ZParameter &dest)
{
  assert(this->isSameType(prev) && this->isSameType(dest));
  const Z3DTransformParameter& prevPara = static_cast<const Z3DTransformParameter&>(prev);
  Z3DTransformParameter& desPara = static_cast<Z3DTransformParameter&>(dest);
  desPara.setScale(glm::mix(prevPara.scale(), scale(), progress));
  if (prevPara.m_rotation.get().yzw() == m_rotation.get().yzw()) {
    desPara.setRotation(glm::mix(prevPara.m_rotation.get(), m_rotation.get(), progress));
  } else {
    desPara.setRotation(glm::mix(prevPara.rotation(), rotation(), float(progress)));
  }
  //desPara.setRotation(glm::vec4(glm::mix(prevPara.m_rotation.get().x, m_rotation.get().x, progress), prevPara.m_rotation.get().yzw()));
  desPara.setTranslation(glm::mix(prevPara.translation(), translation(), progress));
  //desPara.setRotationCenter(glm::mix(prevPara.m_center.get(), m_center.get(), progress));
  desPara.setRotationCenter(progress >= 1.0 ? m_center.get() : prevPara.m_center.get());
}

void Z3DTransformParameter::updateMatrix()
{
  if (m_receiveWidgetSignal) {
    glm::mat4 trans1 = glm::translate(glm::mat4(1.f), -m_center.get() * m_scale.get());
    glm::mat4 trans = glm::translate(glm::mat4(1.f), m_translation.get() + m_center.get() * m_scale.get());
    glm::mat4 scale = glm::scale(glm::mat4(1.f), m_scale.get());
    glm::mat4 rot = glm::mat4_cast(rotation());

    m_value = trans * rot * trans1 * scale;

    emit valueChanged();
  }
}

void Z3DTransformParameter::showTransformMatrix()
{
  LINFO() << "Transform:" << m_value;
  LINFO() << "Inverse Transform: " << glm::affineInverse(m_value);
  LINFO() << "";
}

QWidget *Z3DTransformParameter::actualCreateWidget(QWidget *parent)
{
  ZWidgetsGroup transform("Transform", 1);
  transform.addChild(m_scale, 1);
  transform.addChild(m_rotation, 1);
  transform.addChild(m_translation, 1);
  transform.addChild(m_center, 1);

  QPushButton *pb = new QPushButton("Show Transform Matrix");
  connect(pb, &QPushButton::clicked, this, &Z3DTransformParameter::showTransformMatrix);
  transform.addChild(*pb, 2);

  QLayout *lw = transform.createLayout(false);
  //QWidget *widget = new QWidget();
  //widget->setLayout(lw);
  QGroupBox *groupBox = new QGroupBox("Transform Parameters", parent);
  groupBox->setLayout(lw);

  //widget->setParent(parent);
  //return widget;
  return groupBox;
}

void Z3DTransformParameter::beforeChange(glm::mat4 &value)
{
  updateWidget(value);
}

void Z3DTransformParameter::updateWidget(const glm::mat4 &value)
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

void Z3DTransformParameter::setSameAs(const ZParameter &rhs)
{
  setValueSameAs(rhs);
  ZParameter::setSameAs(rhs);
}

QJsonValue Z3DTransformParameter::jsonValue() const
{
  QJsonObject obj;
  m_scale.write(obj);
  m_translation.write(obj);
  m_rotation.write(obj);
  m_center.write(obj);
  return obj;
}

void Z3DTransformParameter::readValue(const QJsonValue &jsonValue)
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
