#pragma once

#include <QSpinBox>

namespace nim {

// prevent spinbox steal mouse wheel event in a scroll area

class ZSpinBoxEventFilter : public QObject
{
public:
  explicit ZSpinBoxEventFilter(QObject* parent = nullptr);

protected:
  virtual bool eventFilter(QObject* obj, QEvent* event) override;
};

class ZSpinBox : public QSpinBox
{
Q_OBJECT
public:
  explicit ZSpinBox(QWidget* parent = nullptr);

  virtual QSize sizeHint() const override;

  virtual QSize minimumSizeHint() const override;

protected:
  virtual void focusInEvent(QFocusEvent* e) override;

  virtual void focusOutEvent(QFocusEvent* e) override;
};


class ZDoubleSpinBox : public QDoubleSpinBox
{
Q_OBJECT
public:
  explicit ZDoubleSpinBox(QWidget* parent = nullptr);

  virtual QSize sizeHint() const override;

  virtual QSize minimumSizeHint() const override;

protected:
  virtual void focusInEvent(QFocusEvent* e) override;

  virtual void focusOutEvent(QFocusEvent* e) override;
};

} // namespace nim

