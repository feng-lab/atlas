#pragma once

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
                                 bool tracking = true, const QString& prefix = "",
                                 const QString& suffix = "", QWidget* parent = nullptr);

  void setValue(int v);

  void setDataRange(int min, int max);

signals:

  void valueChanged(int);

protected:
  void createWidget(int value, int min, int max, int step, bool tracking, const QString& prefix,
                    const QString& suffix);

private:
  void valueChangedFromScrollBar(int v);

  void valueChangedFromSpinBox(int v);

private:
  QScrollBar* m_scrollBar;
  ZSpinBox* m_spinBox;
  QLabel* m_label;
};

} // namespace nim

