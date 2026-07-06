#pragma once

#include "zspinbox.h"
#include <QSlider>

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QStyleOptionSlider;
class QStylePainter;
class QWheelEvent;

namespace nim {

class ZSpanSlider : public QSlider
{
  Q_OBJECT
  Q_PROPERTY(int lowerValue READ lowerValue WRITE setLowerValue)
  Q_PROPERTY(int upperValue READ upperValue WRITE setUpperValue)
  Q_PROPERTY(int lowerPosition READ lowerPosition WRITE setLowerPosition)
  Q_PROPERTY(int upperPosition READ upperPosition WRITE setUpperPosition)
  Q_PROPERTY(HandleMovementMode handleMovementMode READ handleMovementMode WRITE setHandleMovementMode)

public:
  explicit ZSpanSlider(QWidget* parent = nullptr);

  explicit ZSpanSlider(Qt::Orientation orientation, QWidget* parent = nullptr);

  enum HandleMovementMode
  {
    FreeMovement,
    NoCrossing,
    NoOverlapping
  };
  Q_ENUM(HandleMovementMode)

  enum SpanHandle
  {
    NoHandle,
    LowerHandle,
    UpperHandle
  };
  Q_ENUM(SpanHandle)

  [[nodiscard]] HandleMovementMode handleMovementMode() const;

  void setHandleMovementMode(HandleMovementMode mode);

  [[nodiscard]] int lowerValue() const;

  [[nodiscard]] int upperValue() const;

  [[nodiscard]] int lowerPosition() const;

  [[nodiscard]] int upperPosition() const;

public Q_SLOTS:
  void setLowerValue(int lower);

  void setUpperValue(int upper);

  void setSpan(int lower, int upper);

  void setLowerPosition(int lower);

  void setUpperPosition(int upper);

  void setLowerValueBlockSignals(int lower);

  void setUpperValueBlockSignals(int upper);

  void setRangeBlockSignals(int min, int max);

Q_SIGNALS:
  void spanChanged(int lower, int upper);

  void lowerValueChanged(int lower);

  void upperValueChanged(int upper);

  void lowerPositionChanged(int lower);

  void upperPositionChanged(int upper);

  void sliderPressed(SpanHandle handle);

protected:
  void keyPressEvent(QKeyEvent* event) override;

  void mousePressEvent(QMouseEvent* event) override;

  void mouseMoveEvent(QMouseEvent* event) override;

  void mouseReleaseEvent(QMouseEvent* event) override;

  void paintEvent(QPaintEvent* event) override;

  void wheelEvent(QWheelEvent* event) override;

private:
  void initialize();

  void updateRange(int min, int max);

  void movePressedHandle();

  void initStyleOptionForHandle(QStyleOptionSlider* option, SpanHandle handle) const;

  [[nodiscard]] QRect handleRect(SpanHandle handle) const;

  [[nodiscard]] int pick(const QPoint& point) const;

  [[nodiscard]] int pixelPosToRangeValue(int position) const;

  [[nodiscard]] SpanHandle hitHandle(const QPoint& position) const;

  void movePressedHandleTo(int newPosition);

  void applySliderAction(SliderAction action, SpanHandle handle);

  [[nodiscard]] int constrainedLowerPosition(int position) const;

  [[nodiscard]] int constrainedUpperPosition(int position) const;

  void commitLowerPosition();

  void commitUpperPosition();

  void drawSpan(QStylePainter* painter, const QRect& rect) const;

  void drawHandle(QStylePainter* painter, SpanHandle handle) const;

private:
  int m_lower = 0;
  int m_upper = 0;
  int m_lowerPosition = 0;
  int m_upperPosition = 0;
  int m_dragOffset = 0;
  int m_dragStartPosition = 0;
  SpanHandle m_lastPressed = NoHandle;
  SpanHandle m_pressedHandle = NoHandle;
  HandleMovementMode m_movement = FreeMovement;
  bool m_firstMovement = false;
  bool m_blockTracking = false;
};

class ZSpanSliderWithSpinBox : public QWidget
{
  Q_OBJECT

public:
  explicit ZSpanSliderWithSpinBox(int lowerValue,
                                  int upperValue,
                                  int min,
                                  int max,
                                  int singleStep = 1,
                                  bool tracking = true,
                                  QWidget* parent = nullptr);

  void setLowerValueBlockSignals(int lower);

  void setUpperValueBlockSignals(int upper);

  void setDataRangeBlockSignals(int min, int max);

  [[nodiscard]] ZSpanSlider* slider() const
  {
    return m_slider;
  }
  [[nodiscard]] ZSpinBox* lowerSpinBox() const
  {
    return m_lowerSpinBox;
  }
  [[nodiscard]] ZSpinBox* upperSpinBox() const
  {
    return m_upperSpinBox;
  }

Q_SIGNALS:
  void lowerValueChanged(int lower);

  void upperValueChanged(int upper);

private:
  void createWidget(int lowerValue, int upperValue, int min, int max, int singleStep, bool tracking);

  void lowerValueChangedFromSlider(int l);

  void upperValueChangedFromSlider(int u);

  void valueChangedFromLowerSpinBox(int l);

  void valueChangedFromUpperSpinBox(int u);

private:
  ZSpanSlider* m_slider = nullptr;
  ZSpinBox* m_lowerSpinBox = nullptr;
  ZSpinBox* m_upperSpinBox = nullptr;
};

class ZDoubleSpanSliderWithSpinBox : public QWidget
{
  Q_OBJECT

public:
  explicit ZDoubleSpanSliderWithSpinBox(double lowerValue,
                                        double upperValue,
                                        double min,
                                        double max,
                                        double singleStep = .01,
                                        int decimal = 3,
                                        bool tracking = true,
                                        QWidget* parent = nullptr);

  void setLowerValueBlockSignals(double lower);

  void setUpperValueBlockSignals(double upper);

  void setDataRangeBlockSignals(double min, double max);

  [[nodiscard]] ZSpanSlider* slider() const
  {
    return m_slider;
  }
  [[nodiscard]] ZDoubleSpinBox* lowerSpinBox() const
  {
    return m_lowerSpinBox;
  }
  [[nodiscard]] ZDoubleSpinBox* upperSpinBox() const
  {
    return m_upperSpinBox;
  }

Q_SIGNALS:
  void lowerValueChanged(double lower);

  void upperValueChanged(double upper);

private:
  void lowerValueChangedFromSlider(int l);

  void upperValueChangedFromSlider(int u);

  void valueChangedFromLowerSpinBox(double l);

  void valueChangedFromUpperSpinBox(double u);

  void createWidget();

private:
  ZSpanSlider* m_slider = nullptr;
  ZDoubleSpinBox* m_lowerSpinBox = nullptr;
  ZDoubleSpinBox* m_upperSpinBox = nullptr;
  double m_lowerValue;
  double m_upperValue;
  double m_min;
  double m_max;
  double m_step;
  int m_decimal;
  bool m_tracking;
  int m_sliderMaxValue;
};

} // namespace nim
