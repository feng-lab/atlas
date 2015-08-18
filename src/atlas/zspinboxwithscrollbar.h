#ifndef ZSPINBOXWITHSCROLLBAR_H
#define ZSPINBOXWITHSCROLLBAR_H

#include <QWidget>
class QScrollBar;
class QLabel;

namespace nim {

class ZSpinBox;

class ZSpinBoxWithScrollBar : public QWidget
{
  Q_OBJECT
public:
  explicit ZSpinBoxWithScrollBar(int value, int min, int max, int step = 1,
                                 bool tracking = true, const QString &prefix = "",
                                 const QString &suffix = "", QWidget *parent = 0);

signals:
  void valueChanged(int);

public slots:
  void setValue(int v);
  void setDataRange(int min, int max);

private slots:
  void valueChangedFromScrollBar(int v);
  void valueChangedFromSpinBox(int v);

protected:
  void createWidget(int value, int min, int max, int step, bool tracking, const QString &prefix,
                    const QString &suffix);

  QScrollBar* m_scrollBar;
  ZSpinBox* m_spinBox;
  QLabel *m_label;
};

} // namespace nim

#endif // ZSPINBOXWITHSCROLLBAR_H
