#ifndef ZSPINBOX_H
#define ZSPINBOX_H

#include <QSpinBox>

namespace nim {

// prevent spinbox steal mouse wheel event in a scroll area

class ZSpinBoxEventFilter : public QObject
{
public:
  ZSpinBoxEventFilter(QObject *parent = 0);
protected:
  virtual bool eventFilter(QObject *obj, QEvent *event) override;
};

class ZSpinBox : public QSpinBox
{
  Q_OBJECT
public:
  ZSpinBox(QWidget* parent = 0);

  virtual QSize sizeHint() const override;
  virtual QSize minimumSizeHint() const override;

public slots:
  void setDataRange(int min, int max);
  void setDataMin(int min) { setMinimum(min); }
  void setDataMax(int max) { setMaximum(max); }

protected:
  virtual void focusInEvent(QFocusEvent *e) override;
  virtual void focusOutEvent(QFocusEvent *e) override;
};


class ZDoubleSpinBox : public QDoubleSpinBox
{
  Q_OBJECT
public:
  ZDoubleSpinBox(QWidget* parent = 0);

  virtual QSize sizeHint() const override;
  virtual QSize minimumSizeHint() const override;

public slots:
  void setDataRange(double min, double max);
  void setDataMin(double min) { setMinimum(min); }
  void setDataMax(double max) { setMaximum(max); }

protected:
  virtual void focusInEvent(QFocusEvent *e) override;
  virtual void focusOutEvent(QFocusEvent *e) override;
};

} // namespace nim

#endif // ZSPINBOX_H
