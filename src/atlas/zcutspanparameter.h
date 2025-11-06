#pragma once

#include "znumericparameter.h"
#include "zoptionparameter.h"
#include "zwidgetsgroup.h"

namespace nim {

// A self-contained cut span parameter with binding behavior.
// Encapsulates: absolute span, binding mode, pin-lower/upper, and normalized fractions.
// Inherits the absolute value shape from ZFloatSpanParameter so existing users
// can read the span directly; updates are reconciled via beforeChange/changeRange.
class ZCutSpanParameter : public ZFloatSpanParameter
{
  Q_OBJECT

public:
  enum class Mode
  {
    Absolute = 0,
    TrackEdges = 1,
    Normalized = 2
  };

  explicit ZCutSpanParameter(const QString& name, glm::vec2 value, float min, float max, QObject* parent = nullptr);

  Mode mode() const
  {
    return static_cast<Mode>(m_mode.associatedData());
  }
  void setMode(Mode m);

  void setPins(bool lower, bool upper);
  bool pinLower() const
  {
    return m_pinLower.get();
  }
  bool pinUpper() const
  {
    return m_pinUpper.get();
  }

  void setNormalized(glm::dvec2 f01);
  glm::dvec2 normalized() const
  {
    return m_normalized.get();
  }

  // Re-evaluate the absolute span using current mode/pins/normalized for new bounds.
  void applyBounds(double min, double max);

  // ZParameter interface

public:
  [[nodiscard]] json::value jsonValue() const override;
  void readValue(const json::value& jsonValue) override;

protected:
  // Reconcile absolute span on every set based on mode and current bounds.
  void beforeChange(glm::vec2& value) override;
  // On range change, re-evaluate absolute span according to the binding.
  void changeRange() override;
  QWidget* actualCreateWidget(QWidget* parent) override;

private:
  void updateUiEnabling();
  void recomputeAbsoluteFromNormalized(glm::vec2& value) const;

private:
  ZStringIntOptionParameter m_mode; // UI: Absolute | Track Edges | Normalized [0..1]
  ZBoolParameter m_pinLower; // TrackEdges: pin lower to min
  ZBoolParameter m_pinUpper; // TrackEdges: pin upper to max
  ZDoubleSpanParameter m_normalized; // Normalized fractions [0,1]
};

} // namespace nim
