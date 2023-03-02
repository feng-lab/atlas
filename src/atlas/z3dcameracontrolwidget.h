#pragma once

#include "z3dcameraparameter.h"
#include <QWidget>

class QSpinBox;

class QPushButton;

namespace nim {

class Z3DRenderingEngine;

class Z3DCameraControlWidget : public QWidget
{
  Q_OBJECT

public:
  explicit Z3DCameraControlWidget(Z3DCameraParameter& camera, Z3DRenderingEngine& engine, QWidget* parent = nullptr);

private:
  void roll();

  void azimuth();

  void yaw();

  void elevation();

  void pitch();

  void focusOn();

  void focusOnIgnoreClipping();

  void pointsTo();

  void pointsToIgnoreClipping();

  void flipView();

  void setXYView();

  void setXZView();

  void setYZView();

  void createWidget();

private:
  Z3DCameraParameter& m_camera;
  Z3DRenderingEngine& m_view;
  QSpinBox* m_rollDegreeSpinBox = nullptr;
  QSpinBox* m_azimuthDegreeSpinBox = nullptr;
  QSpinBox* m_yawDegreeSpinBox = nullptr;
  QSpinBox* m_elevationDegreeSpinBox = nullptr;
  QSpinBox* m_pitchDegreeSpinBox = nullptr;
  QPushButton* m_focusOnButton = nullptr;
  QPushButton* m_focusOnIgnoreClippingButton = nullptr;
  QPushButton* m_moveCenterButton = nullptr;
  QPushButton* m_moveCenterIgnoreClippingButton = nullptr;
  QPushButton* m_flipViewButton = nullptr;
  QPushButton* m_setXYViewButton = nullptr;
  QPushButton* m_setXZViewButton = nullptr;
  QPushButton* m_setYZViewButton = nullptr;
};

} // namespace nim
