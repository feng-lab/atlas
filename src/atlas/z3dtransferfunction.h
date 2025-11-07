#pragma once

#include "zcolormap.h"
#include "zparameter.h"
#include <QObject>
#include <memory>
#include <vector>

namespace nim {

class ZImg;

// only support 1d transfer function now
class Z3DTransferFunction : public ZColorMap
{
  Q_OBJECT

public:
  explicit Z3DTransferFunction(double min = 0.0,
                               double max = 1.0,
                               const glm::col4& minColor = glm::col4(0, 0, 0, 0),
                               const glm::col4& maxColor = glm::col4(255, 255, 255, 255),
                               uint32_t width = 256,
                               QObject* parent = nullptr);

  Z3DTransferFunction(const Z3DTransferFunction& tf);

  Z3DTransferFunction(Z3DTransferFunction&& tf) noexcept;

  void swap(Z3DTransferFunction& other) noexcept;

  Z3DTransferFunction& operator=(Z3DTransferFunction other) noexcept
  {
    swap(other);
    return *this;
  }

  bool operator==(const Z3DTransferFunction& tf) const;

  void resetToDefault();

  QString samplerType() const;

  glm::uvec3 dimensions() const
  {
    return m_dimensions;
  }

  void resize(uint32_t width);

  // domain should be in [0.0, 1.0] range
  bool isValidDomainMin(double min) const override;

  bool isValidDomainMax(double max) const override;

private:
  // Adapts the given width and height of transfer function to graphics board capabilities.
  void fitDimensions(uint32_t& width, uint32_t& height, uint32_t& depth) const;

protected:
  glm::uvec3 m_dimensions;
};

class Z3DTransferFunctionParameter : public ZSingleValueParameter<Z3DTransferFunction>
{
  Q_OBJECT

public:
  explicit Z3DTransferFunctionParameter(const QString& name, QObject* parent = nullptr);

  Z3DTransferFunctionParameter(const QString& name,
                               double min,
                               double max,
                               const glm::col4& minColor,
                               const glm::col4& maxColor,
                               int width,
                               QObject* parent = nullptr);

  void setImage(std::shared_ptr<const ZImg> image);

  [[nodiscard]] std::shared_ptr<const ZImg> image() const
  {
    return m_image;
  }

  [[nodiscard]] int bitsStored() const
  {
    return m_bitsStored;
  }

  void setMinMaxIntensity(double minInten, double maxInten)
  {
    CHECK(maxInten > minInten);
    m_minIntensity = minInten;
    m_maxIntensity = maxInten;
  }

  [[nodiscard]] double minIntensity() const
  {
    return m_minIntensity != std::numeric_limits<double>::lowest() ? m_minIntensity : m_value.domainMin();
  }

  [[nodiscard]] double maxIntensity() const
  {
    return m_maxIntensity != std::numeric_limits<double>::lowest() ? m_maxIntensity : m_value.domainMax();
  }

  // ZParameter interface

public:
  void setSameAs(const ZParameter& rhs) override;

  [[nodiscard]] bool supportInterpolation() const override
  {
    return false;
  }

  [[nodiscard]] json::value jsonValue() const override;

  void readValue(const json::value& jsonValue) override;

  [[nodiscard]] json::object valueSchema() const override
  {
    // Same shape as ZColorMapParameter: array of key objects
    auto makeColor = []() {
      json::object colorArraySchema;
      colorArraySchema["type"] = "array";
      json::object item; item["type"] = "number";
      colorArraySchema["items"] = item;
      colorArraySchema["minItems"] = 4;
      colorArraySchema["maxItems"] = 4;
      json::object asString; asString["type"] = "string";
      json::object any; json::array arr; arr.emplace_back(asString); arr.emplace_back(colorArraySchema); any["anyOf"] = arr;
      return any;
    };
    json::object keyObj;
    json::object props;
    props["intensity"] = json::object{{"type", "number"}};
    props["colorL"] = makeColor();
    props["colorR"] = makeColor();
    props["split"] = json::object{{"type", "boolean"}};
    keyObj["type"] = "object";
    keyObj["properties"] = props;
    json::array req; req.emplace_back("intensity"); req.emplace_back("colorL"); req.emplace_back("colorR"); req.emplace_back("split");
    keyObj["required"] = req;
    keyObj["additionalProperties"] = false;
    json::object arr;
    arr["type"] = "array";
    arr["items"] = keyObj;
    return arr;
  }

protected:
  QWidget* actualCreateWidget(QWidget* parent) override;

protected:
  std::shared_ptr<const ZImg> m_image;
  int m_bitsStored = 0;
  double m_minIntensity = std::numeric_limits<double>::lowest();
  double m_maxIntensity = std::numeric_limits<double>::lowest();
};

} // namespace nim
