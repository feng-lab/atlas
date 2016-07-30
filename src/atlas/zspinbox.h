#ifndef ZSPINBOX_H
#define ZSPINBOX_H

#include <QSpinBox>

namespace nim {

// prevent spinbox steal mouse wheel event in a scroll area

class ZSpinBoxEventFilter : public QObject
{
public:
  ZSpinBoxEventFilter(QObject* parent = 0);

protected:
  virtual bool eventFilter(QObject* obj, QEvent* event) override;
};

class ZSpinBox : public QSpinBox
{
Q_OBJECT
public:
  ZSpinBox(QWidget* parent = 0);

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
  ZDoubleSpinBox(QWidget* parent = 0);

  virtual QSize sizeHint() const override;

  virtual QSize minimumSizeHint() const override;

protected:
  virtual void focusInEvent(QFocusEvent* e) override;

  virtual void focusOutEvent(QFocusEvent* e) override;
};

} // namespace nim

#endif // ZSPINBOX_H
