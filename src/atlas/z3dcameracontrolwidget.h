#ifndef Z3DCAMERACONTROLWIDGET_H
#define Z3DCAMERACONTROLWIDGET_H

#include <QWidget>
#include "z3dcameraparameter.h"

class QSpinBox;

namespace nim {

class Z3DCameraControlWidget : public QWidget
{
  Q_OBJECT
public:
  explicit Z3DCameraControlWidget(Z3DCameraParameter &camera, QWidget *parent = 0);

signals:

public slots:

private slots:
  void roll();
  void azimuth();
  void yaw();
  void elevation();
  void pitch();

private:
  void createWidget();

private:
  Z3DCameraParameter& m_camera;
  QSpinBox *m_rollDegreeSpinBox;
  QSpinBox *m_azimuthDegreeSpinBox;
  QSpinBox *m_yawDegreeSpinBox;
  QSpinBox *m_elevationDegreeSpinBox;
  QSpinBox *m_pitchDegreeSpinBox;
};

} // namespace nim

#endif // Z3DCAMERACONTROLWIDGET_H
