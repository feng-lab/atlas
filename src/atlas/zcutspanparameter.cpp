#include "zcutspanparameter.h"

#include "zwidgetsgroup.h"
#include <QVBoxLayout>
#include <QGroupBox>
#include <QLabel>

namespace nim {

ZCutSpanParameter::ZCutSpanParameter(const QString& name, glm::vec2 value, float min, float max, QObject* parent)
  : ZFloatSpanParameter(name, value, min, max, parent)
  , m_mode(QStringLiteral("Mode"))
  , m_pinLower(QStringLiteral("Pin Lower"), true)
  , m_pinUpper(QStringLiteral("Pin Upper"), true)
  , m_normalized(QStringLiteral("Normalized [0..1]"), glm::dvec2(0.0, 1.0), 0.0, 1.0)
{
  // Populate mode options: 0=Absolute,1=TrackEdges,2=Normalized
  m_mode.clearOptions();
  m_mode.addOptionsWithData(std::make_pair(QStringLiteral("Absolute"), 0),
                            std::make_pair(QStringLiteral("Track Edges"), 1),
                            std::make_pair(QStringLiteral("Normalized [0..1]"), 2));
  m_mode.select(QStringLiteral("Track Edges"));

  // Default: show full range and follow edges
  m_pinLower.setValue(true);
  m_pinUpper.setValue(true);

  m_normalized.setSingleStep(0.001);
  // Rich descriptions for LLMs and UI help
  this->setDescription(QStringLiteral(
    "Cut span bundle with labeled fields:\n"
    "- Range FloatSpan: absolute [lower, upper] in world units; clamped to [min, max].\n"
    "- Mode StringIntOption: one of {Absolute, Track Edges, Normalized [0..1]}.\n"
    "- Pin Lower Bool / Pin Upper Bool: Track Edges only — pin endpoints to moving min/max on bounds change.\n"
    "- Normalized [0..1] DoubleSpan: fractions f0,f1 used by Normalized mode.\n\n"
    "Evaluation formulas on bounds change (applyBounds):\n"
    "- Absolute: newLower = clamp(oldLower, min, max); newUpper = clamp(oldUpper, min, max).\n"
    "- Track Edges: newLower = (PinLower ? min : clamp(oldLower, min, max));\n"
    "               newUpper = (PinUpper ? max : clamp(oldUpper, min, max)).\n"
    "- Normalized [0..1]: newLower = min + (max-min)*f0; newUpper = min + (max-min)*f1.\n\n"
    "Defaults: Mode = Track Edges; Pin Lower = ON; Pin Upper = ON (shows full range and follows edges).\n"
    "Tip: To keep a proportional window, choose Normalized and set fractions (e.g., 0.1 to 0.9)."));

  // Range row is labeled in the widget layout; no separate proxy parameter
  m_mode.setDescription(QStringLiteral(
    "Binding behavior when bounds change: Absolute | Track Edges | Normalized [0..1]. See parameter description for formulas."));
  m_pinLower.setDescription(QStringLiteral(
    "Track Edges: when ON, the lower endpoint pins to the axis minimum as bounds move; when OFF, it holds its absolute value (clamped)."));
  m_pinUpper.setDescription(QStringLiteral(
    "Track Edges: when ON, the upper endpoint pins to the axis maximum as bounds move; when OFF, it holds its absolute value (clamped)."));
  m_normalized.setDescription(QStringLiteral(
    "Fractions f0,f1 in [0,1] used by Normalized mode. 0 = current min; 1 = current max; auto-ordered."));

  // No proxy sync needed

  connect(&m_mode, &ZStringIntOptionParameter::valueChanged, this, [this]() {
    updateUiEnabling();
    // Range might already be valid; re-apply binding to the current span
    glm::vec2 v = this->get();
    beforeChange(v);
    this->set(v);
  });
  connect(&m_pinLower, &ZBoolParameter::valueChanged, this, [this]() {
    if (mode() == Mode::TrackEdges) {
      glm::vec2 v = this->get();
      beforeChange(v);
      this->set(v);
    }
  });
  connect(&m_pinUpper, &ZBoolParameter::valueChanged, this, [this]() {
    if (mode() == Mode::TrackEdges) {
      glm::vec2 v = this->get();
      beforeChange(v);
      this->set(v);
    }
  });
  connect(&m_normalized, &ZDoubleSpanParameter::valueChanged, this, [this]() {
    if (mode() == Mode::Normalized) {
      glm::vec2 v = this->get();
      beforeChange(v);
      this->set(v);
    }
  });

  updateUiEnabling();
}

void ZCutSpanParameter::setMode(Mode m)
{
  int want = 0;
  switch (m) {
    case Mode::Absolute:
      want = 0;
      break;
    case Mode::TrackEdges:
      want = 1;
      break;
    case Mode::Normalized:
      want = 2;
      break;
  }
  if (m_mode.associatedData() != want) {
    if (want == 0) {
      m_mode.select(QStringLiteral("Absolute"));
    } else if (want == 1) {
      m_mode.select(QStringLiteral("Track Edges"));
    } else {
      m_mode.select(QStringLiteral("Normalized [0..1]"));
    }
  }
}

void ZCutSpanParameter::setPins(bool lower, bool upper)
{
  if (m_pinLower.get() != lower) {
    m_pinLower.setValue(lower);
  }
  if (m_pinUpper.get() != upper) {
    m_pinUpper.setValue(upper);
  }
}

void ZCutSpanParameter::setNormalized(glm::dvec2 f01)
{
  // clamp
  f01[0] = std::clamp(f01[0], 0.0, 1.0);
  f01[1] = std::clamp(f01[1], 0.0, 1.0);
  m_normalized.set(glm::dvec2(std::min(f01[0], f01[1]), std::max(f01[0], f01[1])));
}

void ZCutSpanParameter::applyBounds(double min, double max)
{
  // Adjust range, then recompute absolute based on binding
  ZFloatSpanParameter::setRange(static_cast<float>(min), static_cast<float>(max));
  glm::vec2 v = this->get();
  beforeChange(v);
  this->set(v);
}

json::value ZCutSpanParameter::jsonValue() const
{
  // Mirror Camera/Transform: write sub-parameters into an object
  json::object obj;
  m_mode.write(obj);
  m_pinLower.write(obj);
  m_pinUpper.write(obj);
  m_normalized.write(obj);
  obj["Range FloatSpan"] = json::value_from(this->get());
  return obj;
}

void ZCutSpanParameter::readValue(const json::value& jsonValue)
{
  // Accept legacy vec2 (just span) or a structured object of sub-parameters
  if (!jsonValue.is_object()) {
    this->set(json::value_to<glm::vec2>(jsonValue));
    return;
  }
  const auto& obj = jsonValue.as_object();
  m_mode.read(obj);
  m_pinLower.read(obj);
  m_pinUpper.read(obj);
  m_normalized.read(obj);
  if (obj.if_contains("Range FloatSpan")) {
    this->set(json::value_to<glm::vec2>(obj.at("Range FloatSpan")));
  }

  // Apply to main value and reconcile according to current binding
  glm::vec2 v = this->get();
  beforeChange(v);
  this->set(v);
}

void ZCutSpanParameter::beforeChange(glm::vec2& value)
{
  const double mn = this->minimum();
  const double mx = this->maximum();
  const auto clampd = [](double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
  };
  switch (mode()) {
    case Mode::Absolute: {
      // base makeValid already clamps/order; nothing extra
      break;
    }
    case Mode::TrackEdges: {
      double lo = value[0];
      double hi = value[1];
      if (m_pinLower.get()) {
        lo = mn;
      } else {
        lo = clampd(lo, mn, mx);
      }
      if (m_pinUpper.get()) {
        hi = mx;
      } else {
        hi = clampd(hi, mn, mx);
      }
      if (lo > hi) {
        std::swap(lo, hi);
      }
      value = glm::vec2(static_cast<float>(lo), static_cast<float>(hi));
      break;
    }
    case Mode::Normalized: {
      recomputeAbsoluteFromNormalized(value);
      break;
    }
  }
}

void ZCutSpanParameter::changeRange()
{
  // When range changes, re-evaluate absolute from mode/pins/normalized
  glm::vec2 v = this->get();
  beforeChange(v);
  // Call base to emit rangeChanged first
  ZFloatSpanParameter::changeRange();
  this->set(v);
}

QWidget* ZCutSpanParameter::actualCreateWidget(QWidget* parent)
{
  // Build with ZWidgetsGroup for labeled rows, and add a Range row as a child widget
  ZWidgetsGroup cutGroup(QStringLiteral("Cut"), 1);
  {
    auto* row = new QWidget(parent);
    auto* h = new QHBoxLayout();
    h->setContentsMargins(0, 0, 0, 0);
    auto* lb = new QLabel(QStringLiteral("Range"), row);
    lb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    lb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QWidget* spanWidget = ZFloatSpanParameter::actualCreateWidget(row);
    h->addWidget(lb);
    h->addWidget(spanWidget, 1);
    row->setLayout(h);
    cutGroup.addChild(*row, 1);
  }
  cutGroup.addChild(m_mode, 1);
  cutGroup.addChild(m_pinLower, 1);
  cutGroup.addChild(m_pinUpper, 1);
  m_normalized.setWidgetOrientation(Qt::Horizontal);
  cutGroup.addChild(m_normalized, 1);

  auto* groupBox = new QGroupBox(this->name(), parent);
  groupBox->setLayout(cutGroup.createLayout(false));
  updateUiEnabling();
  return groupBox;
}

void ZCutSpanParameter::updateUiEnabling()
{
  const bool track = mode() == Mode::TrackEdges;
  const bool norm = mode() == Mode::Normalized;
  m_pinLower.setEnabled(track);
  m_pinUpper.setEnabled(track);
  m_normalized.setEnabled(norm);
}

void ZCutSpanParameter::recomputeAbsoluteFromNormalized(glm::vec2& value) const
{
  const double mn = this->minimum();
  const double mx = this->maximum();
  const double range = mx - mn;
  if (range <= 0.0) {
    value = glm::vec2(static_cast<float>(mn), static_cast<float>(mx));
    return;
  }
  const auto f = m_normalized.get();
  const double lo = mn + range * std::clamp(static_cast<double>(f[0]), 0.0, 1.0);
  const double hi = mn + range * std::clamp(static_cast<double>(f[1]), 0.0, 1.0);
  value = glm::vec2(static_cast<float>(std::min(lo, hi)), static_cast<float>(std::max(lo, hi)));
}

} // namespace nim
