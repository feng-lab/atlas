#pragma once

#include <QSpinBox>

namespace nim {

// prevent spinbox steal mouse wheel event in a scroll area

class ZSpinBoxEventFilter : public QObject
{
public:
  explicit ZSpinBoxEventFilter(QObject* parent = nullptr);

protected:
  bool eventFilter(QObject* obj, QEvent* event) override;
};

class ZSpinBox : public QSpinBox
{
  Q_OBJECT

public:
  explicit ZSpinBox(QWidget* parent = nullptr);

  QSize sizeHint() const override;

  QSize minimumSizeHint() const override;

  void setValueBlockSignals(int v);

protected:
  void focusInEvent(QFocusEvent* e) override;

  void focusOutEvent(QFocusEvent* e) override;
};

class ZDoubleSpinBox : public QDoubleSpinBox
{
  Q_OBJECT

public:
  explicit ZDoubleSpinBox(QWidget* parent = nullptr);

  QSize sizeHint() const override;

  QSize minimumSizeHint() const override;

  void setValueBlockSignals(double v);

  void setRangeBlockSignals(double min, double max);

protected:
  void focusInEvent(QFocusEvent* e) override;

  void focusOutEvent(QFocusEvent* e) override;
};

} // namespace nim
