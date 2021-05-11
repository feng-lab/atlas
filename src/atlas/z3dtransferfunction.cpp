#include "z3dtransferfunction.h"

#include "z3dgpuinfo.h"
#include "z3dshaderprogram.h"
#include "z3dtransferfunctionwidgetwitheditorwindow.h"
#include "z3dvolume.h"
#include "z3dtexture.h"
#include "zlog.h"
#include <QLabel>
#include <memory>

namespace nim {

Z3DTransferFunction::Z3DTransferFunction(double min, double max, const glm::col4& minColor,
                                         const glm::col4& maxColor, uint32_t width, QObject* parent)
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

Z3DTransferFunction::Z3DTransferFunction(Z3DTransferFunction&& tf) noexcept
{
  swap(tf);
}

void Z3DTransferFunction::swap(Z3DTransferFunction& other) noexcept
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
  reset(0., 1., glm::col4(0, 0, 0, 0), glm::col4(255, 255, 255, 255));
  Q_EMIT changed();
}

Z3DTexture* Z3DTransferFunction::texture() const
{
  if (m_textureIsInvalid)
    updateTexture();

  return m_texture.get();
}

QString Z3DTransferFunction::samplerType() const
{
  if (m_dimensions.z > 1)
    return "sampler3D";
  if (m_dimensions.y > 1)
    return "sampler2D";

  return "sampler1D";
}

void Z3DTransferFunction::resize(uint32_t width)
{
  fitDimensions(width, m_dimensions.y, m_dimensions.z);

  if (width != m_dimensions.x) {
    m_dimensions.x = width;
    Q_EMIT changed();
  }
}

void Z3DTransferFunction::fitDimensions(uint32_t& width, uint32_t& height, uint32_t& depth) const
{
  uint32_t maxTexSize;
  if (depth == 1) {
    maxTexSize = Z3DGpuInfo::instance().maxTextureSize();
  } else {
    maxTexSize = Z3DGpuInfo::instance().max3DTextureSize();
  }

  if (maxTexSize < width)
    width = maxTexSize;

  if (maxTexSize < height)
    height = maxTexSize;

  if (maxTexSize < depth)
    depth = maxTexSize;
}

void Z3DTransferFunction::updateTexture() const
{
  if (!m_texture || (m_texture->dimension() != glm::uvec3(m_dimensions)))
    createTexture();
  CHECK(m_texture);

  std::vector<glm::col4> tfData(m_dimensions.x);
  for (size_t x = 0; x < tfData.size(); ++x)
    tfData[x] = mappedColorBGRA(static_cast<double>(x) / (tfData.size() - 1.));
  m_texture->uploadImage(tfData.data());

  m_textureIsInvalid = false;
}

void Z3DTransferFunction::createTexture() const
{
  m_texture = std::make_unique<Z3DTexture>(GLint(GL_RGBA8), glm::uvec3(m_dimensions), m_textureFormat, m_textureDataType);
}

bool Z3DTransferFunction::isValidDomainMin(double min) const
{
  return min < domainMax() && min >= 0.0 && min < 1.0;
}

bool Z3DTransferFunction::isValidDomainMax(double max) const
{
  return max > domainMin() && max > 0.0 && max <= 1.0;
}

Z3DTransferFunctionParameter::Z3DTransferFunctionParameter(const QString& name, QObject* parent)
  : ZSingleValueParameter<Z3DTransferFunction>(name, parent)
  , m_volume(nullptr)
{
  connect(&m_value, &Z3DTransferFunction::changed, this, &Z3DTransferFunctionParameter::valueChanged);
}

Z3DTransferFunctionParameter::Z3DTransferFunctionParameter(const QString& name, double min, double max,
                                                           const glm::col4& minColor,
                                                           const glm::col4& maxColor, int width, QObject* parent)
  : ZSingleValueParameter<Z3DTransferFunction>(name, parent)
  , m_volume(nullptr)
{
  m_value = Z3DTransferFunction(min, max, minColor, maxColor, width);
  connect(&m_value, &Z3DTransferFunction::changed, this, &Z3DTransferFunctionParameter::valueChanged);
}

void Z3DTransferFunctionParameter::setVolume(Z3DVolume* volume)
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
    Q_EMIT valueChanged();
  }
}

QWidget* Z3DTransferFunctionParameter::actualCreateWidget(QWidget* parent)
{
  return new Z3DTransferFunctionWidgetWithEditorWindow(this, parent);
}

void Z3DTransferFunctionParameter::setSameAs(const ZParameter& rhs)
{
  CHECK(this->isSameType(rhs));
  const auto* src = static_cast<const Z3DTransferFunctionParameter*>(&rhs);
  if (m_value != src->get()) {
    m_value = src->get();
    m_value.invalidateTexture();
    Q_EMIT valueChanged();
  }
  ZParameter::setSameAs(rhs);
}

json::value Z3DTransferFunctionParameter::jsonValue() const
{
  json::array keyArray;
  for (const auto& k : m_value.m_keys) {
    keyArray.push_back({
                         {"intensity", k.first.m_intensity},
                         {"colorL",    json::value_from(k.first.m_colorL)},
                         {"colorR",    json::value_from(k.first.m_colorR)},
                         {"split",     k.first.m_split},
                       });
  }
  return keyArray;
}

void Z3DTransferFunctionParameter::readValue(const json::value& jsonValue)
{
  m_value.m_keys.clear();
  const auto& keyArray = jsonValue.as_array();
  for (const auto& jv : keyArray) {
    const auto& keyObj = jv.as_object();
    ZColorMapKey key(0, glm::col4());
    if (keyObj.contains("intensity") &&
        keyObj.contains("colorL") &&
        keyObj.contains("colorR") &&
        keyObj.contains("split")) {
      if (keyObj.at("intensity").is_string()) {
        toVal(asQString(keyObj.at("intensity")), key.m_intensity);
        toVal(asQString(keyObj.at("colorL")), key.m_colorL);
        toVal(asQString(keyObj.at("colorR")), key.m_colorR);
      } else {
        key.m_intensity = json::value_to<double>(keyObj.at("intensity"));
        key.m_colorL = json::value_to<glm::col4>(keyObj.at("colorL"));
        key.m_colorR = json::value_to<glm::col4>(keyObj.at("colorR"));
      }
      key.m_split = keyObj.at("split").as_bool();
      m_value.m_keys.emplace_back(key, false);
    } else {
      LOG(WARNING) << "Invalid transfer function key " << jsonToFormattedQString(keyObj);
    }
  }
  m_value.invalidateTexture();
  Q_EMIT valueChanged();
}

} // namespace nim
