#include "zspanslider.h"

#include <QBoxLayout>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStylePainter>
#include <QWheelEvent>
#include <algorithm>

namespace nim {
namespace {

constexpr int kNoOverlapGap = 1;

// Fixed UI resolution for mapping continuous spinbox values onto QSlider's
// integer domain. It does not cap the represented double range.
constexpr int kDoubleSpanSliderResolution = 1000000;

} // namespace

ZSpanSlider::ZSpanSlider(QWidget* parent)
  : QSlider(parent)
{
  initialize();
}

ZSpanSlider::ZSpanSlider(Qt::Orientation orientation, QWidget* parent)
  : QSlider(orientation, parent)
{
  initialize();
}

void ZSpanSlider::initialize()
{
  connect(this, &QSlider::rangeChanged, this, &ZSpanSlider::updateRange);
  setFocusPolicy(Qt::StrongFocus);
}

ZSpanSlider::HandleMovementMode ZSpanSlider::handleMovementMode() const
{
  return m_movement;
}

void ZSpanSlider::setHandleMovementMode(HandleMovementMode mode)
{
  m_movement = mode;
}

int ZSpanSlider::lowerValue() const
{
  return m_lower;
}

int ZSpanSlider::upperValue() const
{
  return m_upper;
}

int ZSpanSlider::lowerPosition() const
{
  return m_lowerPosition;
}

int ZSpanSlider::upperPosition() const
{
  return m_upperPosition;
}

void ZSpanSlider::setLowerValue(int lower)
{
  setSpan(lower, m_upper);
}

void ZSpanSlider::setUpperValue(int upper)
{
  setSpan(m_lower, upper);
}

void ZSpanSlider::setSpan(int lower, int upper)
{
  const int low = qBound(minimum(), std::min(lower, upper), maximum());
  const int high = qBound(minimum(), std::max(lower, upper), maximum());
  const bool lowerChanged = (low != m_lower);
  const bool upperChanged = (high != m_upper);

  if (!lowerChanged && !upperChanged) {
    return;
  }

  m_lower = low;
  m_upper = high;
  m_lowerPosition = low;
  m_upperPosition = high;

  if (lowerChanged) {
    Q_EMIT lowerValueChanged(m_lower);
  }
  if (upperChanged) {
    Q_EMIT upperValueChanged(m_upper);
  }
  Q_EMIT spanChanged(m_lower, m_upper);
  update();
}

void ZSpanSlider::setLowerPosition(int lower)
{
  const int position = constrainedLowerPosition(lower);
  if (position == m_lowerPosition) {
    return;
  }

  m_lowerPosition = position;
  if (isSliderDown()) {
    Q_EMIT lowerPositionChanged(m_lowerPosition);
  }

  if (hasTracking() && !m_blockTracking) {
    commitLowerPosition();
  } else {
    update();
  }
}

void ZSpanSlider::setUpperPosition(int upper)
{
  const int position = constrainedUpperPosition(upper);
  if (position == m_upperPosition) {
    return;
  }

  m_upperPosition = position;
  if (isSliderDown()) {
    Q_EMIT upperPositionChanged(m_upperPosition);
  }

  if (hasTracking() && !m_blockTracking) {
    commitUpperPosition();
  } else {
    update();
  }
}

void ZSpanSlider::setLowerValueBlockSignals(int lower)
{
  const QSignalBlocker blocker(this);
  setLowerValue(lower);
}

void ZSpanSlider::setUpperValueBlockSignals(int upper)
{
  const QSignalBlocker blocker(this);
  setUpperValue(upper);
}

void ZSpanSlider::setRangeBlockSignals(int min, int max)
{
  const QSignalBlocker blocker(this);
  setRange(min, max);
  updateRange(minimum(), maximum());
}

void ZSpanSlider::keyPressEvent(QKeyEvent* event)
{
  SliderAction action = SliderNoAction;
  SpanHandle handle = NoHandle;
  switch (event->key()) {
    case Qt::Key_Left:
      action = !invertedAppearance() ? SliderSingleStepSub : SliderSingleStepAdd;
      handle = (orientation() == Qt::Horizontal) ? LowerHandle : UpperHandle;
      break;
    case Qt::Key_Right:
      action = !invertedAppearance() ? SliderSingleStepAdd : SliderSingleStepSub;
      handle = (orientation() == Qt::Horizontal) ? LowerHandle : UpperHandle;
      break;
    case Qt::Key_Up:
      action = invertedControls() ? SliderSingleStepSub : SliderSingleStepAdd;
      handle = (orientation() == Qt::Vertical) ? LowerHandle : UpperHandle;
      break;
    case Qt::Key_Down:
      action = invertedControls() ? SliderSingleStepAdd : SliderSingleStepSub;
      handle = (orientation() == Qt::Vertical) ? LowerHandle : UpperHandle;
      break;
    case Qt::Key_Home:
      action = SliderToMinimum;
      handle = LowerHandle;
      break;
    case Qt::Key_End:
      action = SliderToMaximum;
      handle = UpperHandle;
      break;
    default:
      QSlider::keyPressEvent(event);
      return;
  }

  applySliderAction(action, handle);
  event->accept();
}

void ZSpanSlider::mousePressEvent(QMouseEvent* event)
{
  if (minimum() == maximum() || (event->buttons() ^ event->button())) {
    event->ignore();
    return;
  }

  const QPoint position = event->position().toPoint();
  const SpanHandle handle = hitHandle(position);
  if (handle != NoHandle) {
    const QRect rect = handleRect(handle);
    m_pressedHandle = handle;
    m_lastPressed = handle;
    m_dragStartPosition = (handle == LowerHandle) ? m_lowerPosition : m_upperPosition;
    m_dragOffset = pick(position - rect.topLeft());
    m_firstMovement = true;
    setSliderDown(true);
    Q_EMIT sliderPressed(handle);
    update(rect);
  }

  event->accept();
}

void ZSpanSlider::mouseMoveEvent(QMouseEvent* event)
{
  if (m_pressedHandle == NoHandle) {
    event->ignore();
    return;
  }

  QStyleOptionSlider option;
  initStyleOptionForHandle(&option, m_pressedHandle);
  int newPosition = pixelPosToRangeValue(pick(event->position().toPoint()) - m_dragOffset);

  const int maxDragDistance = style()->pixelMetric(QStyle::PM_MaximumDragDistance, &option, this);
  if (maxDragDistance >= 0) {
    const QRect dragRect = rect().adjusted(-maxDragDistance, -maxDragDistance, maxDragDistance, maxDragDistance);
    if (!dragRect.contains(event->position().toPoint())) {
      newPosition = m_dragStartPosition;
    }
  }

  if (m_firstMovement) {
    if (m_lower == m_upper && m_pressedHandle == UpperHandle && newPosition < m_lower) {
      m_pressedHandle = LowerHandle;
      m_lastPressed = LowerHandle;
    } else if (m_lower == m_upper && m_pressedHandle == LowerHandle && newPosition > m_upper) {
      m_pressedHandle = UpperHandle;
      m_lastPressed = UpperHandle;
    }
    m_firstMovement = false;
  }

  movePressedHandleTo(newPosition);
  event->accept();
}

void ZSpanSlider::mouseReleaseEvent(QMouseEvent* event)
{
  if (m_pressedHandle != NoHandle) {
    movePressedHandle();
  }

  setSliderDown(false);
  m_pressedHandle = NoHandle;
  update();
  event->accept();
}

void ZSpanSlider::paintEvent(QPaintEvent* event)
{
  Q_UNUSED(event);

  QStylePainter painter(this);

  QStyleOptionSlider option;
  initStyleOptionForHandle(&option, UpperHandle);
  option.sliderPosition = minimum();
  option.sliderValue = minimum();
  option.subControls = QStyle::SC_SliderGroove;
  if (tickPosition() != NoTicks) {
    option.subControls |= QStyle::SC_SliderTickmarks;
  }
  painter.drawComplexControl(QStyle::CC_Slider, option);

  const QRect lowerRect = handleRect(LowerHandle);
  const QRect upperRect = handleRect(UpperHandle);
  const int lowerCenter = pick(lowerRect.center());
  const int upperCenter = pick(upperRect.center());
  const int first = std::min(lowerCenter, upperCenter);
  const int second = std::max(lowerCenter, upperCenter);
  const QPoint center = QRect(lowerRect.center(), upperRect.center()).center();

  QRect spanRect;
  if (orientation() == Qt::Horizontal) {
    spanRect = QRect(QPoint(first, center.y() - 2), QPoint(second, center.y() + 1));
  } else {
    spanRect = QRect(QPoint(center.x() - 2, first), QPoint(center.x() + 1, second));
  }
  drawSpan(&painter, spanRect);

  if (m_lastPressed == LowerHandle) {
    drawHandle(&painter, UpperHandle);
    drawHandle(&painter, LowerHandle);
  } else {
    drawHandle(&painter, LowerHandle);
    drawHandle(&painter, UpperHandle);
  }
}

void ZSpanSlider::wheelEvent(QWheelEvent* event)
{
  event->ignore();
}

void ZSpanSlider::updateRange(int min, int max)
{
  Q_UNUSED(min);
  Q_UNUSED(max);
  setSpan(m_lower, m_upper);
}

void ZSpanSlider::movePressedHandle()
{
  if (m_pressedHandle == LowerHandle && m_lowerPosition != m_lower) {
    commitLowerPosition();
  } else if (m_pressedHandle == UpperHandle && m_upperPosition != m_upper) {
    commitUpperPosition();
  }
}

void ZSpanSlider::initStyleOptionForHandle(QStyleOptionSlider* option, SpanHandle handle) const
{
  initStyleOption(option);
  if (handle == LowerHandle) {
    option->sliderPosition = m_lowerPosition;
    option->sliderValue = m_lower;
  } else {
    option->sliderPosition = m_upperPosition;
    option->sliderValue = m_upper;
  }
}

QRect ZSpanSlider::handleRect(SpanHandle handle) const
{
  QStyleOptionSlider option;
  initStyleOptionForHandle(&option, handle);
  return style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
}

int ZSpanSlider::pick(const QPoint& point) const
{
  return orientation() == Qt::Horizontal ? point.x() : point.y();
}

int ZSpanSlider::pixelPosToRangeValue(int position) const
{
  QStyleOptionSlider option;
  initStyleOptionForHandle(&option, UpperHandle);

  int sliderMin = 0;
  int sliderMax = 0;
  int sliderLength = 0;
  const QRect grooveRect = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
  const QRect sliderRect = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
  if (orientation() == Qt::Horizontal) {
    sliderLength = sliderRect.width();
    sliderMin = grooveRect.x();
    sliderMax = grooveRect.right() - sliderLength + 1;
  } else {
    sliderLength = sliderRect.height();
    sliderMin = grooveRect.y();
    sliderMax = grooveRect.bottom() - sliderLength + 1;
  }

  if (sliderMax <= sliderMin) {
    return minimum();
  }

  return QStyle::sliderValueFromPosition(minimum(),
                                         maximum(),
                                         position - sliderMin,
                                         sliderMax - sliderMin,
                                         option.upsideDown);
}

ZSpanSlider::SpanHandle ZSpanSlider::hitHandle(const QPoint& position) const
{
  QStyleOptionSlider option;
  initStyleOptionForHandle(&option, UpperHandle);
  if (style()->hitTestComplexControl(QStyle::CC_Slider, &option, position, this) == QStyle::SC_SliderHandle) {
    return UpperHandle;
  }

  initStyleOptionForHandle(&option, LowerHandle);
  if (style()->hitTestComplexControl(QStyle::CC_Slider, &option, position, this) == QStyle::SC_SliderHandle) {
    return LowerHandle;
  }

  return NoHandle;
}

void ZSpanSlider::movePressedHandleTo(int newPosition)
{
  if (m_pressedHandle == LowerHandle) {
    if (m_movement == FreeMovement && newPosition > m_upperPosition) {
      m_pressedHandle = UpperHandle;
      m_lastPressed = UpperHandle;
      setSpan(m_upper, newPosition);
    } else {
      setLowerPosition(newPosition);
    }
  } else if (m_pressedHandle == UpperHandle) {
    if (m_movement == FreeMovement && newPosition < m_lowerPosition) {
      m_pressedHandle = LowerHandle;
      m_lastPressed = LowerHandle;
      setSpan(newPosition, m_lower);
    } else {
      setUpperPosition(newPosition);
    }
  }
}

void ZSpanSlider::applySliderAction(SliderAction action, SpanHandle handle)
{
  if (handle == NoHandle || action == SliderNoAction) {
    return;
  }

  int position = (handle == LowerHandle) ? m_lowerPosition : m_upperPosition;
  switch (action) {
    case SliderSingleStepAdd:
      position += singleStep();
      break;
    case SliderSingleStepSub:
      position -= singleStep();
      break;
    case SliderToMinimum:
      position = minimum();
      break;
    case SliderToMaximum:
      position = maximum();
      break;
    case SliderMove:
      break;
    default:
      return;
  }

  const bool oldBlockTracking = m_blockTracking;
  m_blockTracking = true;
  if (handle == LowerHandle) {
    setLowerPosition(position);
  } else {
    setUpperPosition(position);
  }
  m_blockTracking = oldBlockTracking;

  if (handle == LowerHandle) {
    commitLowerPosition();
  } else {
    commitUpperPosition();
  }
}

int ZSpanSlider::constrainedLowerPosition(int position) const
{
  int result = qBound(minimum(), position, maximum());
  if (m_movement == NoCrossing) {
    result = std::min(result, m_upper);
  } else if (m_movement == NoOverlapping) {
    result = std::min(result, std::max(minimum(), m_upper - kNoOverlapGap));
  }
  return qBound(minimum(), result, maximum());
}

int ZSpanSlider::constrainedUpperPosition(int position) const
{
  int result = qBound(minimum(), position, maximum());
  if (m_movement == NoCrossing) {
    result = std::max(result, m_lower);
  } else if (m_movement == NoOverlapping) {
    result = std::max(result, std::min(maximum(), m_lower + kNoOverlapGap));
  }
  return qBound(minimum(), result, maximum());
}

void ZSpanSlider::commitLowerPosition()
{
  setSpan(m_lowerPosition, m_upper);
}

void ZSpanSlider::commitUpperPosition()
{
  setSpan(m_lower, m_upperPosition);
}

void ZSpanSlider::drawSpan(QStylePainter* painter, const QRect& rect) const
{
  QStyleOptionSlider option;
  initStyleOptionForHandle(&option, UpperHandle);
  QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
  if (option.orientation == Qt::Horizontal) {
    groove.adjust(0, 0, -1, 0);
  } else {
    groove.adjust(0, 0, 0, -1);
  }

  const QRect clipped = rect.intersected(groove);
  if (clipped.isEmpty()) {
    return;
  }

  const QColor highlight = palette().color(QPalette::Highlight);
  painter->save();
  painter->setPen(QPen(highlight.darker(130), 0));
  if (option.orientation == Qt::Horizontal) {
    QLinearGradient gradient(groove.center().x(), groove.top(), groove.center().x(), groove.bottom());
    gradient.setColorAt(0, highlight.darker(115));
    gradient.setColorAt(1, highlight.lighter(110));
    painter->setBrush(gradient);
  } else {
    QLinearGradient gradient(groove.left(), groove.center().y(), groove.right(), groove.center().y());
    gradient.setColorAt(0, highlight.darker(115));
    gradient.setColorAt(1, highlight.lighter(110));
    painter->setBrush(gradient);
  }
  painter->drawRect(clipped);
  painter->restore();
}

void ZSpanSlider::drawHandle(QStylePainter* painter, SpanHandle handle) const
{
  QStyleOptionSlider option;
  initStyleOptionForHandle(&option, handle);
  option.subControls = QStyle::SC_SliderHandle;
  if (m_pressedHandle == handle) {
    option.activeSubControls = QStyle::SC_SliderHandle;
    option.state |= QStyle::State_Sunken;
  }
  painter->drawComplexControl(QStyle::CC_Slider, option);
}

ZSpanSliderWithSpinBox::ZSpanSliderWithSpinBox(int lowerValue,
                                               int upperValue,
                                               int min,
                                               int max,
                                               int singleStep,
                                               bool tracking,
                                               QWidget* parent)
  : QWidget(parent)
{
  createWidget(lowerValue, upperValue, min, max, singleStep, tracking);
}

void ZSpanSliderWithSpinBox::setLowerValueBlockSignals(int lower)
{
  m_slider->setLowerValueBlockSignals(lower);
  m_lowerSpinBox->setValueBlockSignals(lower);
}

void ZSpanSliderWithSpinBox::setUpperValueBlockSignals(int upper)
{
  m_slider->setUpperValueBlockSignals(upper);
  m_upperSpinBox->setValueBlockSignals(upper);
}

void ZSpanSliderWithSpinBox::setDataRangeBlockSignals(int min, int max)
{
  m_slider->setRangeBlockSignals(min, max);
  m_lowerSpinBox->setRangeBlockSignals(m_slider->minimum(), m_slider->upperValue());
  // m_lowerSpinBox->setValueBlockSignals(m_slider->lowerValue());
  m_upperSpinBox->setRangeBlockSignals(m_slider->lowerValue(), m_slider->maximum());
  // m_upperSpinBox->setValueBlockSignals(m_slider->upperValue());
}

void ZSpanSliderWithSpinBox::lowerValueChangedFromSlider(int lower)
{
  m_lowerSpinBox->setValueBlockSignals(lower);
  m_upperSpinBox->setMinimum(lower);
  Q_EMIT lowerValueChanged(lower);
}

void ZSpanSliderWithSpinBox::upperValueChangedFromSlider(int upper)
{
  m_upperSpinBox->setValueBlockSignals(upper);
  m_lowerSpinBox->setMaximum(upper);
  Q_EMIT upperValueChanged(upper);
}

void ZSpanSliderWithSpinBox::valueChangedFromLowerSpinBox(int lower)
{
  m_slider->setLowerValueBlockSignals(lower);
  m_upperSpinBox->setMinimum(lower);
  Q_EMIT lowerValueChanged(lower);
}

void ZSpanSliderWithSpinBox::valueChangedFromUpperSpinBox(int upper)
{
  m_slider->setUpperValueBlockSignals(upper);
  m_lowerSpinBox->setMaximum(upper);
  Q_EMIT upperValueChanged(upper);
}

void ZSpanSliderWithSpinBox::createWidget(int lowerValue,
                                          int upperValue,
                                          int min,
                                          int max,
                                          int singleStep,
                                          bool tracking)
{
  m_slider = new ZSpanSlider(Qt::Horizontal);
  m_slider->setRange(min, max);
  m_slider->setSpan(lowerValue, upperValue);
  m_slider->setSingleStep(singleStep);
  m_slider->setTracking(tracking);
  m_slider->setHandleMovementMode(ZSpanSlider::NoOverlapping);
  m_slider->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  m_lowerSpinBox = new ZSpinBox();
  m_lowerSpinBox->setRange(min, upperValue);
  m_lowerSpinBox->setValueBlockSignals(lowerValue);
  m_lowerSpinBox->setSingleStep(singleStep);
  m_upperSpinBox = new ZSpinBox();
  m_upperSpinBox->setRange(lowerValue, max);
  m_upperSpinBox->setValueBlockSignals(upperValue);
  m_upperSpinBox->setSingleStep(singleStep);
  auto lo = new QHBoxLayout(this);
  lo->setContentsMargins(0, 0, 0, 0);
  lo->addWidget(m_lowerSpinBox);
  lo->addWidget(m_slider);
  lo->addWidget(m_upperSpinBox);
  connect(m_slider, &ZSpanSlider::lowerValueChanged, this, &ZSpanSliderWithSpinBox::lowerValueChangedFromSlider);
  connect(m_slider, &ZSpanSlider::upperValueChanged, this, &ZSpanSliderWithSpinBox::upperValueChangedFromSlider);
  connect(m_lowerSpinBox,
          qOverload<int>(&ZSpinBox::valueChanged),
          this,
          &ZSpanSliderWithSpinBox::valueChangedFromLowerSpinBox);
  connect(m_upperSpinBox,
          qOverload<int>(&ZSpinBox::valueChanged),
          this,
          &ZSpanSliderWithSpinBox::valueChangedFromUpperSpinBox);
}

ZDoubleSpanSliderWithSpinBox::ZDoubleSpanSliderWithSpinBox(double lowerValue,
                                                           double upperValue,
                                                           double min,
                                                           double max,
                                                           double singleStep,
                                                           int decimal,
                                                           bool tracking,
                                                           QWidget* parent)
  : QWidget(parent)
  , m_lowerValue(lowerValue)
  , m_upperValue(upperValue)
  , m_min(min)
  , m_max(max)
  , m_step(singleStep)
  , m_decimal(decimal)
  , m_tracking(tracking)
{
  m_sliderMaxValue = kDoubleSpanSliderResolution;
  createWidget();
}

void ZDoubleSpanSliderWithSpinBox::setLowerValueBlockSignals(double lower)
{
  const double sliderPosition = (lower - m_min) / (m_max - m_min) * m_sliderMaxValue;
  m_slider->setLowerValueBlockSignals(sliderPosition);
  m_lowerSpinBox->setValueBlockSignals(lower);
}

void ZDoubleSpanSliderWithSpinBox::setUpperValueBlockSignals(double upper)
{
  const double sliderPosition = (upper - m_min) / (m_max - m_min) * m_sliderMaxValue;
  m_slider->setUpperValueBlockSignals(sliderPosition);
  m_upperSpinBox->setValueBlockSignals(upper);
}

void ZDoubleSpanSliderWithSpinBox::setDataRangeBlockSignals(double min, double max)
{
  m_min = min;
  m_max = max;
  const double lower = m_slider->lowerValue() / static_cast<double>(m_sliderMaxValue) * (m_max - m_min) + m_min;
  const double upper = m_slider->upperValue() / static_cast<double>(m_sliderMaxValue) * (m_max - m_min) + m_min;
  m_lowerSpinBox->setRangeBlockSignals(m_min, upper);
  // m_lowerSpinBox->setValueBlockSignals(l);
  m_upperSpinBox->setRangeBlockSignals(lower, m_max);
  // m_upperSpinBox->setValueBlockSignals(u);
}

void ZDoubleSpanSliderWithSpinBox::lowerValueChangedFromSlider(int lower)
{
  m_lowerValue = lower / static_cast<double>(m_sliderMaxValue) * (m_max - m_min) + m_min;
  m_lowerSpinBox->setValueBlockSignals(m_lowerValue);
  m_upperSpinBox->setMinimum(m_lowerValue);
  Q_EMIT lowerValueChanged(m_lowerValue);
}

void ZDoubleSpanSliderWithSpinBox::upperValueChangedFromSlider(int upper)
{
  m_upperValue = upper / static_cast<double>(m_sliderMaxValue) * (m_max - m_min) + m_min;
  m_upperSpinBox->setValueBlockSignals(m_upperValue);
  m_lowerSpinBox->setMaximum(m_upperValue);
  Q_EMIT upperValueChanged(m_upperValue);
}

void ZDoubleSpanSliderWithSpinBox::valueChangedFromLowerSpinBox(double lower)
{
  m_lowerValue = lower;
  const int sliderPosition = static_cast<int>((m_lowerValue - m_min) / (m_max - m_min) * m_sliderMaxValue);
  m_slider->setLowerValueBlockSignals(sliderPosition);
  m_upperSpinBox->setMinimum(m_lowerValue);
  Q_EMIT lowerValueChanged(m_lowerValue);
}

void ZDoubleSpanSliderWithSpinBox::valueChangedFromUpperSpinBox(double upper)
{
  m_upperValue = upper;
  const int sliderPosition = static_cast<int>((m_upperValue - m_min) / (m_max - m_min) * m_sliderMaxValue);
  m_slider->setUpperValueBlockSignals(sliderPosition);
  m_lowerSpinBox->setMaximum(m_upperValue);
  Q_EMIT upperValueChanged(m_upperValue);
}

void ZDoubleSpanSliderWithSpinBox::createWidget()
{
  m_slider = new ZSpanSlider(Qt::Horizontal);
  m_slider->setRange(0, m_sliderMaxValue);
  m_slider->setSpan(static_cast<int>((m_lowerValue - m_min) / (m_max - m_min) * m_sliderMaxValue),
                    static_cast<int>((m_upperValue - m_min) / (m_max - m_min) * m_sliderMaxValue));
  m_slider->setSingleStep(std::max(1, static_cast<int>(m_step * m_sliderMaxValue / (m_max - m_min))));
  m_slider->setTracking(m_tracking);
  m_slider->setHandleMovementMode(ZSpanSlider::NoOverlapping);
  m_slider->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  m_lowerSpinBox = new ZDoubleSpinBox();
  m_lowerSpinBox->setRange(m_min, m_upperValue);
  m_lowerSpinBox->setValueBlockSignals(m_lowerValue);
  m_lowerSpinBox->setSingleStep(m_step);
  m_lowerSpinBox->setDecimals(m_decimal);
  m_upperSpinBox = new ZDoubleSpinBox();
  m_upperSpinBox->setRange(m_lowerValue, m_max);
  m_upperSpinBox->setValueBlockSignals(m_upperValue);
  m_upperSpinBox->setSingleStep(m_step);
  m_upperSpinBox->setDecimals(m_decimal);
  auto lo = new QHBoxLayout(this);
  lo->setContentsMargins(0, 0, 0, 0);
  lo->addWidget(m_lowerSpinBox);
  lo->addWidget(m_slider);
  lo->addWidget(m_upperSpinBox);
  connect(m_slider, &ZSpanSlider::lowerValueChanged, this, &ZDoubleSpanSliderWithSpinBox::lowerValueChangedFromSlider);
  connect(m_slider, &ZSpanSlider::upperValueChanged, this, &ZDoubleSpanSliderWithSpinBox::upperValueChangedFromSlider);
  connect(m_lowerSpinBox,
          qOverload<double>(&ZDoubleSpinBox::valueChanged),
          this,
          &ZDoubleSpanSliderWithSpinBox::valueChangedFromLowerSpinBox);
  connect(m_upperSpinBox,
          qOverload<double>(&ZDoubleSpinBox::valueChanged),
          this,
          &ZDoubleSpanSliderWithSpinBox::valueChangedFromUpperSpinBox);
}

} // namespace nim
