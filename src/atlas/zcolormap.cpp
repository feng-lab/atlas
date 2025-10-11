#include "zcolormap.h"

#include "zglmutils.h"
#include "zlog.h"
#include "zcolormapwidgetwitheditorwindow.h"
#include <QWidget>
#include <algorithm>
#include <limits>
#include <memory>

namespace nim {

ZColorMapKey::ZColorMapKey(double i, const glm::col4& color)
  : m_intensity(i)
  , m_colorL(color)
  , m_colorR(color)
  , m_split(false)
{}

ZColorMapKey::ZColorMapKey(double i, const glm::col4& colorL, const glm::col4& colorR)
  : m_intensity(i)
  , m_colorL(colorL)
  , m_colorR(colorR)
  , m_split(true)
{}

ZColorMapKey::ZColorMapKey(double i, const glm::vec4& color)
  : m_intensity(i)
  , m_colorL(glm::col4(color * 255.f))
  , m_colorR(glm::col4(color * 255.f))
  , m_split(false)
{}

ZColorMapKey::ZColorMapKey(double i, const glm::vec4& colorL, const glm::vec4& colorR)
  : m_intensity(i)
  , m_colorL(glm::col4(colorL * 255.f))
  , m_colorR(glm::col4(colorR * 255.f))
  , m_split(true)
{}

ZColorMapKey::ZColorMapKey(double i, const QColor& color)
  : m_intensity(i)
  , m_colorL(color.red(), color.green(), color.blue(), color.alpha())
  , m_colorR(color.red(), color.green(), color.blue(), color.alpha())
  , m_split(false)
{}

ZColorMapKey::ZColorMapKey(double i, const QColor& colorL, const QColor& colorR)
  : m_intensity(i)
  , m_colorL(colorL.red(), colorL.green(), colorL.blue(), colorL.alpha())
  , m_colorR(colorR.red(), colorR.green(), colorR.blue(), colorR.alpha())
  , m_split(true)
{}

void ZColorMapKey::setColorL(const glm::col4& color)
{
  m_colorL = color;
  if (!m_split) {
    m_colorR = m_colorL;
  }
}

void ZColorMapKey::setColorL(const glm::ivec4& color)
{
  m_colorL = glm::col4(color);
  if (!m_split) {
    m_colorR = m_colorL;
  }
}

void ZColorMapKey::setColorL(const glm::vec4& color)
{
  m_colorL = glm::col4(color * 255.f);
  if (!m_split) {
    m_colorR = m_colorL;
  }
}

void ZColorMapKey::setColorL(const QColor& color)
{
  m_colorL = glm::col4(color.red(), color.green(), color.blue(), color.alpha());
  if (!m_split) {
    m_colorR = m_colorL;
  }
}

QColor ZColorMapKey::qColorL() const
{
  return QColor(m_colorL.r, m_colorL.g, m_colorL.b, m_colorL.a);
}

void ZColorMapKey::setColorR(const glm::col4& color)
{
  m_colorR = color;
  if (!m_split) {
    m_colorL = m_colorR;
  }
}

void ZColorMapKey::setColorR(const glm::ivec4& color)
{
  m_colorR = glm::col4(color);
  if (!m_split) {
    m_colorL = m_colorR;
  }
}

void ZColorMapKey::setColorR(const glm::vec4& color)
{
  m_colorR = glm::col4(color * 255.f);
  if (!m_split) {
    m_colorL = m_colorR;
  }
}

void ZColorMapKey::setColorR(const QColor& color)
{
  m_colorR = glm::col4(color.red(), color.green(), color.blue(), color.alpha());
  if (!m_split) {
    m_colorL = m_colorR;
  }
}

QColor ZColorMapKey::qColorR() const
{
  return QColor(m_colorR.r, m_colorR.g, m_colorR.b, m_colorR.a);
}

void ZColorMapKey::setSplit(bool split, bool useLeft)
{
  if (m_split == split) {
    return;
  }
  if (!split) {
    if (useLeft) {
      m_colorR = m_colorL;
    } else {
      m_colorL = m_colorR;
    }
  }
  m_split = split;
}

void ZColorMapKey::setFloatAlphaR(double a)
{
  m_colorR.a = static_cast<uint8_t>(a * 255.);
  if (!m_split) {
    m_colorL.a = m_colorR.a;
  }
}

void ZColorMapKey::setFloatAlphaL(double a)
{
  m_colorL.a = static_cast<uint8_t>(a * 255.);
  if (!m_split) {
    m_colorR.a = m_colorL.a;
  }
}

void ZColorMapKey::setAlphaR(uint8_t a)
{
  m_colorR.a = a;
  if (!m_split) {
    m_colorL.a = m_colorR.a;
  }
}

void ZColorMapKey::setAlphaL(uint8_t a)
{
  m_colorL.a = a;
  if (!m_split) {
    m_colorR.a = m_colorL.a;
  }
}

double ZColorMapKey::floatAlphaR() const
{
  return m_colorR.a / 255.;
}

double ZColorMapKey::floatAlphaL() const
{
  return m_colorL.a / 255.;
}

uint8_t ZColorMapKey::alphaR() const
{
  return m_colorR.a;
}

uint8_t ZColorMapKey::alphaL() const
{
  return m_colorL.a;
}

ZColorMapKey* ZColorMapKey::clone() const
{
  if (!m_split) {
    return new ZColorMapKey(m_intensity, m_colorL);
  } else {
    return new ZColorMapKey(m_intensity, m_colorL, m_colorR);
  }
}

ZColorMap::ZColorMap(double min, double max, const glm::col4& minColor, const glm::col4& maxColor, QObject* parent)
  : QObject(parent)
  , m_hasDataRange(false)
  , m_dataMin(0)
  , m_dataMax(0)
{
  addKey(ZColorMapKey(min, minColor));
  max = std::max(max, min + std::numeric_limits<double>::epsilon());
  addKey(ZColorMapKey(max, maxColor));
  connect(this, &ZColorMap::changed, this, &ZColorMap::invalidateTexture);
}

ZColorMap::ZColorMap(double min, double max, const glm::vec4& minColor, const glm::vec4& maxColor, QObject* parent)
  : QObject(parent)
  , m_hasDataRange(false)
  , m_dataMin(0)
  , m_dataMax(0)
{
  addKey(ZColorMapKey(min, minColor));
  max = std::max(max, min + std::numeric_limits<double>::epsilon());
  addKey(ZColorMapKey(max, maxColor));
  connect(this, &ZColorMap::changed, this, &ZColorMap::invalidateTexture);
}

ZColorMap::ZColorMap(double min, double max, const QColor& minColor, const QColor& maxColor, QObject* parent)
  : QObject(parent)
  , m_hasDataRange(false)
  , m_dataMin(0)
  , m_dataMax(0)
{
  addKey(ZColorMapKey(min, minColor));
  max = std::max(max, min + std::numeric_limits<double>::epsilon());
  addKey(ZColorMapKey(max, maxColor));
  connect(this, &ZColorMap::changed, this, &ZColorMap::invalidateTexture);
}

ZColorMap::ZColorMap(const ZColorMap& cm)
  : QObject(cm.parent())
  , m_hasDataRange(cm.m_hasDataRange)
  , m_dataMin(cm.m_dataMin)
  , m_dataMax(cm.m_dataMax)
{
  m_keys = cm.m_keys;
  connect(this, &ZColorMap::changed, this, &ZColorMap::invalidateTexture);
}

void ZColorMap::buildLUTBGRA8(std::vector<uint8_t>& out, uint32_t width) const
{
  if (width == 0u) {
    out.clear();
    return;
  }
  out.resize(static_cast<size_t>(width) * 4u);
  for (uint32_t x = 0; x < width; ++x) {
    const double t = width > 1 ? static_cast<double>(x) / static_cast<double>(width - 1u) : 0.0;
    const glm::col4 c = mappedColorBGRA(t);
    const size_t idx = static_cast<size_t>(x) * 4u;
    out[idx + 0] = static_cast<uint8_t>(c.r);
    out[idx + 1] = static_cast<uint8_t>(c.g);
    out[idx + 2] = static_cast<uint8_t>(c.b);
    out[idx + 3] = static_cast<uint8_t>(c.a);
  }
}

void ZColorMap::buildLUTRGBA8(std::vector<uint8_t>& out, uint32_t width) const
{
  if (width == 0u) {
    out.clear();
    return;
  }
  out.resize(static_cast<size_t>(width) * 4u);
  for (uint32_t x = 0; x < width; ++x) {
    const double t = width > 1 ? static_cast<double>(x) / static_cast<double>(width - 1u) : 0.0;
    const glm::col4 c = mappedColor(t);
    const size_t idx = static_cast<size_t>(x) * 4u;
    out[idx + 0] = static_cast<uint8_t>(c.r);
    out[idx + 1] = static_cast<uint8_t>(c.g);
    out[idx + 2] = static_cast<uint8_t>(c.b);
    out[idx + 3] = static_cast<uint8_t>(c.a);
  }
}

ZColorMap::ZColorMap(ZColorMap&& other) noexcept
{
  swap(other);
}

void ZColorMap::swap(ZColorMap& other) noexcept
{
  m_keys.swap(other.m_keys);
  std::swap(m_hasDataRange, other.m_hasDataRange);
  std::swap(m_dataMin, other.m_dataMin);
  std::swap(m_dataMax, other.m_dataMax);
  std::swap(m_generation, other.m_generation);
}

void ZColorMap::reset(double min, double max, const glm::col4& minColor, const glm::col4& maxColor)
{
  m_hasDataRange = false;
  blockSignals(true);
  clearKeys();
  addKey(ZColorMapKey(min, minColor));
  max = std::max(max, min + std::numeric_limits<double>::epsilon());
  addKey(ZColorMapKey(max, maxColor));
  m_dataMin = 0;
  m_dataMax = 0;
  blockSignals(false);
  Q_EMIT changed();
}

void ZColorMap::reset(double min, double max, const glm::vec4& minColor, const glm::vec4& maxColor)
{
  m_hasDataRange = false;
  blockSignals(true);
  clearKeys();
  addKey(ZColorMapKey(min, minColor));
  max = std::max(max, min + std::numeric_limits<double>::epsilon());
  addKey(ZColorMapKey(max, maxColor));
  m_dataMin = 0;
  m_dataMax = 0;
  blockSignals(false);
  Q_EMIT changed();
}

void ZColorMap::reset(double min, double max, const QColor& minColor, const QColor& maxColor)
{
  m_hasDataRange = false;
  blockSignals(true);
  clearKeys();
  addKey(ZColorMapKey(min, minColor));
  max = std::max(max, min + std::numeric_limits<double>::epsilon());
  addKey(ZColorMapKey(max, maxColor));
  m_dataMin = 0;
  m_dataMax = 0;
  blockSignals(false);
  Q_EMIT changed();
}

bool ZColorMap::operator==(const ZColorMap& cm) const
{
  return equalTo(cm);
}

double ZColorMap::domainMin() const
{
  return m_keys[0].first.intensity();
}

double ZColorMap::domainMax() const
{
  return m_keys.back().first.intensity();
}

bool ZColorMap::isValidDomainMin(double min) const
{
  return (!m_hasDataRange && min < domainMax()) || (m_hasDataRange && min <= m_dataMin);
}

bool ZColorMap::isValidDomainMax(double max) const
{
  return (!m_hasDataRange && max > domainMin()) || (m_hasDataRange && max >= m_dataMax);
}

bool ZColorMap::setDomainMin(double min, bool rescaleKeys)
{
  if (min == domainMin()) {
    return true;
  }
  if (isValidDomainMin(min)) {
    blockSignals(true);
    if (rescaleKeys) {
      double prevDistToMax = domainMax() - domainMin();
      double distToMax = domainMax() - min;
      double scale = distToMax / prevDistToMax;
      double dmax = domainMax();
      std::vector<std::pair<ZColorMapKey, bool>> newKeys;
      for (size_t i = 0; i < m_keys.size(); ++i) {
        double inten = keyIntensity(i);
        double newInten = dmax - (dmax - inten) * scale;
        if (isKeySplit(i)) {
          newKeys.emplace_back(ZColorMapKey(newInten, keyColorL(i), keyColorR(i)), m_keys[i].second);
        } else {
          newKeys.emplace_back(ZColorMapKey(newInten, keyColorL(i)), m_keys[i].second);
        }
      }
      m_keys = newKeys;
    } else {
      glm::col4 col = mappedColor(min);
      size_t startIdx = m_keys.size();
      size_t endIdx = 0;
      for (size_t i = 0; i < m_keys.size(); ++i) {
        if (keyIntensity(i) <= min) {
          startIdx = std::min(startIdx, i);
          endIdx = std::max(endIdx, i);
        } else {
          break;
        }
      }
      if (startIdx < m_keys.size()) {
        m_keys.erase(m_keys.begin() + startIdx, m_keys.begin() + endIdx + 1);
      }

      addKey(ZColorMapKey(min, col));
    }
    blockSignals(false);
    Q_EMIT changed();
    return true;
  }
  return false;
}

bool ZColorMap::setDomainMax(double max, bool rescaleKeys)
{
  if (max == domainMax()) {
    return true;
  }
  if (isValidDomainMax(max)) {
    blockSignals(true);
    if (rescaleKeys) {
      double prevDistToMin = domainMax() - domainMin();
      double distToMin = max - domainMin();
      double scale = distToMin / prevDistToMin;
      double dmin = domainMin();
      std::vector<std::pair<ZColorMapKey, bool>> newKeys;
      for (size_t i = 0; i < m_keys.size(); ++i) {
        double inten = keyIntensity(i);
        double newInten = dmin + (inten - dmin) * scale;
        if (isKeySplit(i)) {
          newKeys.emplace_back(ZColorMapKey(newInten, keyColorL(i), keyColorR(i)), m_keys[i].second);
        } else {
          newKeys.emplace_back(ZColorMapKey(newInten, keyColorL(i)), m_keys[i].second);
        }
      }
      m_keys = newKeys;
    } else {
      glm::col4 col = mappedColor(max);
      size_t startIdx = m_keys.size();
      size_t endIdx = 0;
      for (size_t i = m_keys.size(); i-- > 0;) {
        if (keyIntensity(i) >= max) {
          startIdx = std::min(startIdx, i);
          endIdx = std::max(endIdx, i);
        } else {
          break;
        }
      }
      if (startIdx < m_keys.size()) {
        m_keys.erase(m_keys.begin() + startIdx, m_keys.begin() + endIdx + 1);
      }

      addKey(ZColorMapKey(max, col));
    }
    blockSignals(false);
    Q_EMIT changed();
    return true;
  }
  return false;
}

void ZColorMap::setDomain(double min, double max, bool rescaleKeys)
{
  if (min >= max) {
    LOG(ERROR) << "wrong input";
    return;
  }
  if (min == domainMin() && max == domainMax()) {
    return;
  }
  if (min < domainMax()) {
    bool ok = false;
    blockSignals(true);
    ok &= setDomainMin(min, rescaleKeys);
    ok &= setDomainMax(max, rescaleKeys);
    blockSignals(false);
    if (ok) {
      Q_EMIT changed();
    }
  } else {
    bool ok = false;
    blockSignals(true);
    ok &= setDomainMax(max, rescaleKeys);
    ok &= setDomainMin(min, rescaleKeys);
    blockSignals(false);
    if (ok) {
      Q_EMIT changed();
    }
  }
}

void ZColorMap::setDomain(const glm::dvec2& domain, bool rescaleKeys)
{
  setDomain(domain.x, domain.y, rescaleKeys);
}

glm::col4 ZColorMap::mappedColor(double i) const
{
  if (m_keys.empty()) {
    return glm::col4(0, 0, 0, 0);
  }

  // iterate through all keys until we get to the correct position
  auto keyIt = m_keys.begin();

  while ((keyIt != m_keys.end()) && (i > (*keyIt).first.intensity())) {
    keyIt++;
  }

  if (keyIt == m_keys.begin()) {
    return m_keys[0].first.colorL();
  } else if (keyIt == m_keys.end()) {
    return (*(keyIt - 1)).first.colorR();
  } else {
    // calculate the value weighted by the destination to the next left and right key
    ZColorMapKey leftKey = (*(keyIt - 1)).first;
    ZColorMapKey rightKey = keyIt->first;
    double fraction = (i - leftKey.intensity()) / (rightKey.intensity() - leftKey.intensity());
    glm::col4 leftDest = leftKey.colorR();
    glm::col4 rightDest = rightKey.colorL();
    glm::col4 result = leftDest;
    result.r += static_cast<uint8_t>((rightDest.r - leftDest.r) * fraction);
    result.g += static_cast<uint8_t>((rightDest.g - leftDest.g) * fraction);
    result.b += static_cast<uint8_t>((rightDest.b - leftDest.b) * fraction);
    result.a += static_cast<uint8_t>((rightDest.a - leftDest.a) * fraction);
    return result;
  }
}

glm::col4 ZColorMap::mappedColorBGRA(double i) const
{
  glm::col4 result = mappedColor(i);
  std::swap(result.r, result.b);
  return result;
}

glm::vec4 ZColorMap::mappedFColor(double i) const
{
  glm::col4 col = mappedColor(i);
  return glm::vec4(col) / 255.f;
}

QColor ZColorMap::mappedQColor(double i) const
{
  glm::col4 col = mappedColor(i);
  return QColor(col.r, col.g, col.b, col.a);
}

glm::col4 ZColorMap::keyColorL(size_t index) const
{
  return m_keys[index].first.colorL();
}

glm::vec4 ZColorMap::keyFColorL(size_t index) const
{
  return glm::vec4(m_keys[index].first.colorL()) / 255.f;
}

QColor ZColorMap::keyQColorL(size_t index) const
{
  return m_keys[index].first.qColorL();
}

glm::col4 ZColorMap::keyColorR(size_t index) const
{
  return m_keys[index].first.colorR();
}

glm::vec4 ZColorMap::keyFColorR(size_t index) const
{
  return glm::vec4(m_keys[index].first.colorR()) / 255.f;
}

QColor ZColorMap::keyQColorR(size_t index) const
{
  return m_keys[index].first.qColorR();
}

void ZColorMap::setKeyColorL(size_t index, const glm::col4& color)
{
  m_keys[index].first.setColorL(color);
  Q_EMIT changed();
}

void ZColorMap::setKeyColorR(size_t index, const glm::col4& color)
{
  m_keys[index].first.setColorR(color);
  Q_EMIT changed();
}

void ZColorMap::setKeyColorL(size_t index, const glm::vec4& color)
{
  m_keys[index].first.setColorL(color);
  Q_EMIT changed();
}

void ZColorMap::setKeyColorR(size_t index, const glm::vec4& color)
{
  m_keys[index].first.setColorR(color);
  Q_EMIT changed();
}

void ZColorMap::setKeyColorL(size_t index, const QColor& color)
{
  m_keys[index].first.setColorL(color);
  Q_EMIT changed();
}

void ZColorMap::setKeyColorR(size_t index, const QColor& color)
{
  m_keys[index].first.setColorR(color);
  Q_EMIT changed();
}

double ZColorMap::keyFloatAlphaL(size_t index) const
{
  return m_keys[index].first.floatAlphaL();
}

uint8_t ZColorMap::keyAlphaL(size_t index) const
{
  return m_keys[index].first.alphaL();
}

double ZColorMap::keyFloatAlphaR(size_t index) const
{
  return m_keys[index].first.floatAlphaR();
}

uint8_t ZColorMap::keyAlphaR(size_t index) const
{
  return m_keys[index].first.alphaR();
}

void ZColorMap::setKeyAlphaL(size_t index, uint8_t a)
{
  m_keys[index].first.setAlphaL(a);
  Q_EMIT changed();
}

void ZColorMap::setKeyAlphaR(size_t index, uint8_t a)
{
  m_keys[index].first.setAlphaR(a);
  Q_EMIT changed();
}

void ZColorMap::setKeyFloatAlphaL(size_t index, double a)
{
  m_keys[index].first.setFloatAlphaL(a);
  Q_EMIT changed();
}

void ZColorMap::setKeyFloatAlphaR(size_t index, double a)
{
  m_keys[index].first.setFloatAlphaR(a);
  Q_EMIT changed();
}

double ZColorMap::keyIntensity(size_t index) const
{
  return m_keys[index].first.intensity();
}

void ZColorMap::setKeyIntensity(size_t index, double intensity)
{
  std::pair<ZColorMapKey, bool> newPair = m_keys[index];
  newPair.first.setIntensity(intensity);
  blockSignals(true);
  removeKey(index);
  addKey(newPair.first, newPair.second);
  blockSignals(false);
  Q_EMIT changed();
}

bool ZColorMap::isKeySelected(size_t index) const
{
  return m_keys[index].second;
}

void ZColorMap::setKeySelected(size_t index, bool v)
{
  if (isKeySelected(index) != v) {
    m_keys[index].second = v;
    Q_EMIT changed();
  }
}

void ZColorMap::deselectAllKeys()
{
  bool change = false;
  for (auto& key : m_keys) {
    if (key.second) {
      change = true;
      key.second = false;
    }
  }
  if (change) {
    Q_EMIT changed();
  }
}

std::vector<size_t> ZColorMap::selectedKeyIndexes() const
{
  std::vector<size_t> all;
  for (size_t i = 0; i < m_keys.size(); ++i) {
    if (m_keys[i].second) {
      all.push_back(i);
    }
  }
  return all;
}

bool ZColorMap::isKeySplit(size_t index) const
{
  return m_keys[index].first.isSplit();
}

void ZColorMap::setKeySplit(size_t index, bool v, bool useLeft)
{
  if (isKeySplit(index) != v) {
    m_keys[index].first.setSplit(v, useLeft);
    Q_EMIT changed();
  }
}

glm::col4 ZColorMap::fractionMappedColor(double fraction) const
{
  double i = domainMin() + fraction * (domainMax() - domainMin());
  return mappedColor(i);
}

glm::vec4 ZColorMap::fractionMappedFColor(double fraction) const
{
  double i = domainMin() + fraction * (domainMax() - domainMin());
  return mappedFColor(i);
}

QColor ZColorMap::fractionMappedQColor(double fraction) const
{
  double i = domainMin() + fraction * (domainMax() - domainMin());
  return mappedQColor(i);
}

bool ZColorMap::setKey(size_t index, const ZColorMapKey& key, bool select)
{
  if (index < m_keys.size()) {
    blockSignals(true);
    removeKey(index);
    addKey(key, select);
    blockSignals(false);
    Q_EMIT changed();
    return true;
  }
  return false;
}

bool ZColorMap::setKeys(const std::vector<ZColorMapKey>& keys)
{
  if (keys.size() < 2) {
    return false;
  }
  blockSignals(true);
  clearKeys();
  for (const auto& key : keys) {
    addKey(key);
  }
  blockSignals(false);
  Q_EMIT changed();
  return true;
}

ZColorMapKey& ZColorMap::addKey(const ZColorMapKey& key, bool select)
{
  if (m_keys.empty()) {
    m_keys.emplace_back(key, select);
    Q_EMIT changed();
    return m_keys.back().first;
  }
  auto keyIt = m_keys.begin();
  // Forward to the correct position
  while ((keyIt != m_keys.end()) && (key.intensity() > (*keyIt).first.intensity())) {
    keyIt++;
  }
  if (keyIt == m_keys.end()) {
    m_keys.emplace_back(key, select);
    Q_EMIT changed();
    return m_keys.back().first;
  } else {
    auto iter = m_keys.emplace(keyIt, key, select);
    Q_EMIT changed();
    return (*iter).first;
  }
}

void ZColorMap::addKeyAtIntensity(double intensity, bool select)
{
  addKey(ZColorMapKey(intensity, mappedColor(intensity)), select);
}

void ZColorMap::addKeyAtIntensity(double intensity, const glm::col4& color, bool select)
{
  addKey(ZColorMapKey(intensity, color), select);
}

void ZColorMap::addKeyAtIntensity(double intensity, uint8_t alpha, bool select)
{
  glm::col4 col = mappedColor(intensity);
  col.a = alpha;
  addKey(ZColorMapKey(intensity, col), select);
}

void ZColorMap::addKeyAtIntensity(double intensity, double alpha, bool select)
{
  glm::vec4 col = mappedFColor(intensity);
  col.a = alpha;
  addKey(ZColorMapKey(intensity, col), select);
}

void ZColorMap::addKeyAtFraction(double fraction, const glm::col4& color, bool select)
{
  double intensity = domainMin() + fraction * (domainMax() - domainMin());
  addKey(ZColorMapKey(intensity, color), select);
}

void ZColorMap::addKeyAtFraction(double fraction, bool select)
{
  glm::col4 col = fractionMappedColor(fraction);
  double intensity = domainMin() + fraction * (domainMax() - domainMin());
  addKey(ZColorMapKey(intensity, col), select);
}

void ZColorMap::addKeyAtFraction(double fraction, uint8_t alpha, bool select)
{
  glm::col4 col = fractionMappedColor(fraction);
  col.a = alpha;
  double intensity = domainMin() + fraction * (domainMax() - domainMin());
  addKey(ZColorMapKey(intensity, col), select);
}

void ZColorMap::addKeyAtFraction(double fraction, double alpha, bool select)
{
  glm::vec4 col = fractionMappedFColor(fraction);
  col.a = alpha;
  double intensity = domainMin() + fraction * (domainMax() - domainMin());
  addKey(ZColorMapKey(intensity, col), select);
}

bool ZColorMap::removeDuplicatedKeys()
{
  size_t sizeBefore = m_keys.size();
  unique(m_keys, false, [](const std::pair<ZColorMapKey, bool>& key1, const std::pair<ZColorMapKey, bool>& key2) {
    return key1.first.intensity() == key2.first.intensity();
  });
  if (m_keys.size() != sizeBefore) {
    Q_EMIT changed();
  }
  return m_keys.size() != sizeBefore;
}

bool ZColorMap::removeSelectedKeys()
{
  size_t sizeBefore = m_keys.size();
  std::erase_if(m_keys, [](const auto& key) {
    return key.second;
  });
  if (m_keys.size() != sizeBefore) {
    Q_EMIT changed();
  }
  return m_keys.size() != sizeBefore;
}

void ZColorMap::updateKeys()
{
  std::ranges::sort(m_keys);
}

void ZColorMap::removeKey(const ZColorMapKey& key)
{
  std::erase(m_keys, std::make_pair(key, false));
  std::erase(m_keys, std::make_pair(key, true));
  Q_EMIT changed();
}

void ZColorMap::removeKey(size_t index)
{
  m_keys.erase(m_keys.begin() + index);
  Q_EMIT changed();
}

ZColorMapParameter::ZColorMapParameter(const QString& name, QObject* parent)
  : ZSingleValueParameter<ZColorMap>(name, parent)
{
  connect(&m_value, &ZColorMap::changed, this, &ZColorMapParameter::valueChanged);
}

ZColorMapParameter::ZColorMapParameter(const QString& name, const ZColorMap& cm, QObject* parent)
  : ZSingleValueParameter<ZColorMap>(name, cm, parent)
{
  connect(&m_value, &ZColorMap::changed, this, &ZColorMapParameter::valueChanged);
}

ZColorMapParameter::ZColorMapParameter(const QString& name,
                                       double min,
                                       double max,
                                       const glm::col4& minColor,
                                       const glm::col4& maxColor,
                                       QObject* parent)
  : ZSingleValueParameter<ZColorMap>(name, parent)
{
  m_value.reset(min, max, minColor, maxColor);
  connect(&m_value, &ZColorMap::changed, this, &ZColorMapParameter::valueChanged);
}

ZColorMapParameter::ZColorMapParameter(const QString& name,
                                       double min,
                                       double max,
                                       const glm::vec4& minColor,
                                       const glm::vec4& maxColor,
                                       QObject* parent)
  : ZSingleValueParameter<ZColorMap>(name, parent)
{
  m_value.reset(min, max, minColor, maxColor);
  connect(&m_value, &ZColorMap::changed, this, &ZColorMapParameter::valueChanged);
}

ZColorMapParameter::ZColorMapParameter(const QString& name,
                                       double min,
                                       double max,
                                       const QColor& minColor,
                                       const QColor& maxColor,
                                       QObject* parent)
  : ZSingleValueParameter<ZColorMap>(name, parent)
{
  m_value.reset(min, max, minColor, maxColor);
  connect(&m_value, &ZColorMap::changed, this, &ZColorMapParameter::valueChanged);
}

QWidget* ZColorMapParameter::actualCreateWidget(QWidget* parent)
{
  return new ZColorMapWidgetWithEditorWindow(this, parent);
}

void ZColorMapParameter::setSameAs(const ZParameter& rhs)
{
  CHECK(this->isSameType(rhs));
  const ZColorMapParameter* src = static_cast<const ZColorMapParameter*>(&rhs);
  if (m_value != src->get()) {
    m_value = src->get();
    Q_EMIT valueChanged();
  }
  ZParameter::setSameAs(rhs);
}

json::value ZColorMapParameter::jsonValue() const
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

void ZColorMapParameter::readValue(const json::value& jsonValue)
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
  Q_EMIT valueChanged();
}

bool ZColorMap::equalTo(const ZColorMap& cm) const
{
  return m_keys == cm.m_keys && m_hasDataRange == cm.m_hasDataRange && m_dataMin == cm.m_dataMin &&
         m_dataMax == cm.m_dataMax;
}

} // namespace nim
