#include "z3dtransferfunction.h"
#include "z3dgpuinfo.h"
#include "z3dshaderprogram.h"
#include "z3dtransferfunctionwidgetwitheditorwindow.h"
#include "z3dvolume.h"
#include "z3dtexture.h"
#include <QLabel>
#include <QJsonArray>

namespace nim {

Z3DTransferFunction::Z3DTransferFunction(double min, double max, const glm::col4 &minColor,
                                         const glm::col4 &maxColor, int width, QObject *parent)
  : ZColorMap(min, max, minColor, maxColor, parent)
  , m_dimensions(width, 1, 1)
  , m_textureFormat(GL_BGRA)
  , m_textureDataType(GL_UNSIGNED_INT_8_8_8_8_REV)
{
}

Z3DTransferFunction::Z3DTransferFunction(const Z3DTransferFunction& tf)
  : ZColorMap(tf)
  , m_dimensions(tf.m_dimensions)
  , m_textureFormat(tf.m_textureFormat)
  , m_textureDataType(tf.m_textureDataType)
{
}

Z3DTransferFunction::Z3DTransferFunction(Z3DTransferFunction &&tf)
{
  swap(tf);
}

Z3DTransferFunction::~Z3DTransferFunction()
{
}

void Z3DTransferFunction::swap(Z3DTransferFunction &other) noexcept
{
  ZColorMap::swap(other);
  std::swap(m_dimensions, other.m_dimensions);
  std::swap(m_textureFormat, other.m_textureFormat);
  std::swap(m_textureDataType, other.m_textureDataType);
}

bool Z3DTransferFunction::operator==(const Z3DTransferFunction& tf) const
{
  if (!ZColorMap::equalTo(tf))
    return false;
  if (m_dimensions != tf.m_dimensions)
    return false;
  if (m_textureDataType != tf.m_textureDataType)
    return false;
  if (m_textureFormat != tf.m_textureFormat)
    return false;

  return true;
}

bool Z3DTransferFunction::operator!=(const Z3DTransferFunction& tf) const
{
  return !(*this == tf);
}

void Z3DTransferFunction::resetToDefault()
{
  reset(0., 1., glm::col4(0,0,0,0), glm::col4(255,255,255,255));
  emit changed();
}

void Z3DTransferFunction::createTexture()
{
  m_texture.reset(new Z3DTexture(m_dimensions, m_textureFormat, (GLint)GL_RGBA8, m_textureDataType));
  CHECK_GL_ERROR;
}

Z3DTexture* Z3DTransferFunction::texture()
{
  if (m_textureIsInvalid)
    updateTexture();

  return m_texture.get();
}

QString Z3DTransferFunction::samplerType() const
{
  if (m_dimensions.z > 1)
    return "sampler3D";
  else if (m_dimensions.y > 1)
    return "sampler2D";
  else
    return "sampler1D";
}

void Z3DTransferFunction::resize(int width)
{
  fitDimensions(width, m_dimensions.y, m_dimensions.z);

  if (width != m_dimensions.x) {
    m_dimensions.x = width;
    emit changed();
  }
}

void Z3DTransferFunction::fitDimensions(int& width, int& height, int& depth) const
{
  int maxTexSize;
  if (depth == 1)
    maxTexSize = Z3DGpuInfoInstance.maxTextureSize();
  else
    maxTexSize = Z3DGpuInfoInstance.max3DTextureSize();

  if (maxTexSize < width)
    width = maxTexSize;

  if (maxTexSize < height)
    height = maxTexSize;

  if (maxTexSize < depth)
    depth = maxTexSize;
}

void Z3DTransferFunction::updateTexture()
{
  if (!m_texture || (m_texture->dimensions() != m_dimensions))
    createTexture();
  assert(m_texture);

  std::vector<glm::col4> tfData(m_dimensions.x);
  for (size_t x = 0; x < tfData.size(); ++x)
    tfData[x] = mappedColorBGRA(static_cast<double>(x) / (tfData.size()-1));
  m_texture->setData(&tfData[0]);

  m_texture->uploadTexture();
  CHECK_GL_ERROR;

  m_textureIsInvalid = false;
}

bool Z3DTransferFunction::isValidDomainMin(double min) const
{
  if (min < domainMax() && min >= 0.0 && min < 1.0)
    return true;
  else
    return false;
}

bool Z3DTransferFunction::isValidDomainMax(double max) const
{
  if (max > domainMin() && max > 0.0 && max <= 1.0)
    return true;
  else
    return false;
}

Z3DTransferFunctionParameter::Z3DTransferFunctionParameter(const QString &name, QObject *parent)
  : ZSingleValueParameter<Z3DTransferFunction>(name, parent)
  , m_volume(NULL)
{
  connect(&m_value, SIGNAL(changed()), this, SIGNAL(valueChanged()));
}

Z3DTransferFunctionParameter::Z3DTransferFunctionParameter(const QString &name, double min, double max, const glm::col4 &minColor,
                                                           const glm::col4 &maxColor, int width, QObject *parent)
  : ZSingleValueParameter<Z3DTransferFunction>(name, parent)
  , m_volume(NULL)
{
  m_value = Z3DTransferFunction(min, max, minColor, maxColor, width);
  connect(&m_value, SIGNAL(changed()), this, SIGNAL(valueChanged()));
}

void Z3DTransferFunctionParameter::setVolume(Z3DVolume *volume)
{
  if (m_volume != volume) {
    m_volume = volume;
    if (m_volume) {
      // Resize texture of tf according to bitdepth of volume
      int bits = m_volume->bitsStored();
      if (bits > 16)
        bits = 16; // handle float data as if it was 16 bit to prevent overflow

      int max = 1 << bits;
      m_value.resize(max);
    }
    emit valueChanged();
  }
}

QWidget *Z3DTransferFunctionParameter::actualCreateWidget(QWidget *parent)
{
  return new Z3DTransferFunctionWidgetWithEditorWindow(this, parent);
}

void Z3DTransferFunctionParameter::setSameAs(const ZParameter &rhs)
{
  assert(this->isSameType(rhs));
  const Z3DTransferFunctionParameter* src = static_cast<const Z3DTransferFunctionParameter*>(&rhs);
  if (m_value != src->get()) {
    m_value = src->get();
    m_value.invalidateTexture();
    emit valueChanged();
  }
  ZParameter::setSameAs(rhs);
}

QJsonValue Z3DTransferFunctionParameter::jsonValue() const
{
  QJsonArray keyArray;
  for (std::vector<std::pair<ZColorMapKey, bool>>::const_iterator it = m_value.m_keys.begin();
       it != m_value.m_keys.end(); ++it) {
    QJsonObject key;
    key.insert("intensity", toQString(it->first.m_intensity));
    key.insert("colorL", toQString(it->first.m_colorL));
    key.insert("colorR", toQString(it->first.m_colorR));
    key.insert("split", it->first.m_split);
    keyArray.append(key);
  }
  return keyArray;
}

void Z3DTransferFunctionParameter::readValue(const QJsonValue &jsonValue)
{
  m_value.m_keys.clear();
  QJsonArray keyArray = jsonValue.toArray();
  for (int i=0; i<keyArray.size(); ++i) {
    QJsonObject keyObj = keyArray[i].toObject();
    ZColorMapKey key(0, glm::col4());
    if (keyObj.contains("intensity") &&
        keyObj.contains("colorL") &&
        keyObj.contains("colorR") &&
        keyObj.contains("split")) {
      toVal(keyObj["intensity"].toString(), key.m_intensity);
      toVal(keyObj["colorL"].toString(), key.m_colorL);
      toVal(keyObj["colorR"].toString(), key.m_colorR);
      key.m_split = keyObj["split"].toBool();
      m_value.m_keys.emplace_back(key, false);
    } else {
      LWARN() << "Invalid transfer function key" << keyObj.keys().join("  ");
    }
  }
  m_value.invalidateTexture();
  emit valueChanged();
}

} // namespace nim
