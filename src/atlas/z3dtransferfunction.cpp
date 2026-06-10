#include "z3dtransferfunction.h"

#include "z3dtransferfunctionwidgetwitheditorwindow.h"
#include "zimg.h"
#include "zlog.h"
#include <QLabel>
#include <algorithm>
#include <memory>

namespace nim {

namespace {

int bitsStoredForImage(const ZImg& image)
{
  const auto& info = image.info();
  const auto bitsFromFormat = static_cast<int>(info.bytesPerVoxel * 8);
  if (info.voxelFormat == VoxelFormat::Float) {
    return bitsFromFormat;
  }
  if (info.validBitCount != 0) {
    return static_cast<int>(info.validBitCount);
  }
  return bitsFromFormat;
}

size_t histogramTextureWidthForBits(int bits)
{
  if (bits <= 0) {
    return 256;
  }
  const int clampedBits = std::clamp(bits, 1, 16);
  return static_cast<size_t>(1) << clampedBits;
}

} // namespace

Z3DTransferFunction::Z3DTransferFunction(double min,
                                         double max,
                                         const glm::col4& minColor,
                                         const glm::col4& maxColor,
                                         uint32_t width,
                                         QObject* parent)
  : ZColorMap(min, max, minColor, maxColor, parent)
  , m_dimensions(width, 1, 1)
{
  captureDefaultFromCurrent();
}

Z3DTransferFunction::Z3DTransferFunction(const Z3DTransferFunction& tf)
  : ZColorMap(tf)
  , m_dimensions(tf.m_dimensions)
  , m_defaultDomain(tf.m_defaultDomain)
  , m_defaultMinColor(tf.m_defaultMinColor)
  , m_defaultMaxColor(tf.m_defaultMaxColor)
{}

Z3DTransferFunction::Z3DTransferFunction(Z3DTransferFunction&& tf) noexcept
{
  swap(tf);
}

void Z3DTransferFunction::swap(Z3DTransferFunction& other) noexcept
{
  ZColorMap::swap(other);
  std::swap(m_dimensions, other.m_dimensions);
  std::swap(m_defaultDomain, other.m_defaultDomain);
  std::swap(m_defaultMinColor, other.m_defaultMinColor);
  std::swap(m_defaultMaxColor, other.m_defaultMaxColor);
}

bool Z3DTransferFunction::operator==(const Z3DTransferFunction& tf) const
{
  if (!ZColorMap::equalTo(tf)) {
    return false;
  }
  if (m_dimensions != tf.m_dimensions) {
    return false;
  }

  return true;
}

void Z3DTransferFunction::resetToDefault()
{
  reset(m_defaultDomain.x, m_defaultDomain.y, m_defaultMinColor, m_defaultMaxColor);
  Q_EMIT changed();
}

void Z3DTransferFunction::captureDefaultFromCurrent()
{
  m_defaultDomain = glm::dvec2(domainMin(), domainMax());

  if (numKeys() >= 2) {
    m_defaultMinColor = keyColorL(0);
    m_defaultMaxColor = keyColorR(numKeys() - 1);
  } else if (numKeys() == 1) {
    m_defaultMinColor = keyColorL(0);
    m_defaultMaxColor = keyColorR(0);
  } else {
    m_defaultMinColor = glm::col4(0, 0, 0, 0);
    m_defaultMaxColor = glm::col4(255, 255, 255, 255);
  }
}

QString Z3DTransferFunction::samplerType() const
{
  if (m_dimensions.z > 1) {
    return "sampler3D";
  }
  if (m_dimensions.y > 1) {
    return "sampler2D";
  }

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
  // CPU-only TF: keep dimensions as requested; clamp to sensible minimums.
  width = std::max<uint32_t>(1, width);
  height = std::max<uint32_t>(1, height);
  depth = std::max<uint32_t>(1, depth);
}

// GL texture APIs removed: renderers build backend textures from CPU LUTs.

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
{
  connect(&m_value, &Z3DTransferFunction::changed, this, &Z3DTransferFunctionParameter::valueChanged);
}

Z3DTransferFunctionParameter::Z3DTransferFunctionParameter(const QString& name,
                                                           double min,
                                                           double max,
                                                           const glm::col4& minColor,
                                                           const glm::col4& maxColor,
                                                           int width,
                                                           QObject* parent)
  : ZSingleValueParameter<Z3DTransferFunction>(name, parent)
{
  m_value = Z3DTransferFunction(min, max, minColor, maxColor, width);
  connect(&m_value, &Z3DTransferFunction::changed, this, &Z3DTransferFunctionParameter::valueChanged);
}

void Z3DTransferFunctionParameter::setImage(std::shared_ptr<const ZImg> image)
{
  if (m_image == image) {
    return;
  }

  m_image = std::move(image);
  if (m_image) {
    m_bitsStored = bitsStoredForImage(*m_image);
    const size_t desiredWidth = histogramTextureWidthForBits(m_bitsStored);
    if (desiredWidth != m_value.dimensions().x) {
      m_value.resize(static_cast<uint32_t>(desiredWidth));
    }
  } else {
    m_bitsStored = 0;
  }

  Q_EMIT valueChanged();
}

QWidget* Z3DTransferFunctionParameter::actualCreateWidget(QWidget* parent)
{
  return new Z3DTransferFunctionWidgetWithEditorWindow(this, parent);
}

void Z3DTransferFunctionParameter::setSameAs(const ZParameter& rhs)
{
  CHECK(this->isSameType(rhs));
  const auto* src = static_cast<const Z3DTransferFunctionParameter*>(&rhs);
  m_image = src->image();
  m_bitsStored = src->bitsStored();
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
      {"intensity", k.first.m_intensity               },
      {"colorL",    json::value_from(k.first.m_colorL)},
      {"colorR",    json::value_from(k.first.m_colorR)},
      {"split",     k.first.m_split                   },
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
    if (keyObj.contains("intensity") && keyObj.contains("colorL") && keyObj.contains("colorR") &&
        keyObj.contains("split")) {
      if (keyObj.at("intensity").is_string()) {
        toVal(keyObj.at("intensity").get_string(), key.m_intensity);
        toVal(keyObj.at("colorL").as_string(), key.m_colorL);
        toVal(keyObj.at("colorR").as_string(), key.m_colorR);
      } else {
        key.m_intensity = keyObj.at("intensity").to_number<double>();
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
