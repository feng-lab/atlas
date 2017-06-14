#include "z3dcameracontrolwidget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSpinBox>

namespace nim {

Z3DCameraControlWidget::Z3DCameraControlWidget(Z3DCameraParameter& camera, QWidget* parent)
  : QWidget(parent)
  , m_camera(camera)
{
  createWidget();
}

void Z3DCameraControlWidget::roll()
{
  if (m_rollDegreeSpinBox->value() % 360 != 0) {
    double angle = glm::radians(double(m_rollDegreeSpinBox->value()));
    m_camera.roll(angle);
  }
}

void Z3DCameraControlWidget::azimuth()
{
  if (m_azimuthDegreeSpinBox->value() % 360 != 0) {
    double angle = glm::radians(double(m_azimuthDegreeSpinBox->value()));
    m_camera.azimuth(angle);
  }
}

void Z3DCameraControlWidget::yaw()
{
  if (m_yawDegreeSpinBox->value() % 360 != 0) {
    double angle = glm::radians(double(m_yawDegreeSpinBox->value()));
    m_camera.yaw(angle);
  }
}

void Z3DCameraControlWidget::elevation()
{
  if (m_elevationDegreeSpinBox->value() % 360 != 0) {
    double angle = glm::radians(double(m_elevationDegreeSpinBox->value()));
    m_camera.elevation(angle);
  }
}

void Z3DCameraControlWidget::pitch()
{
  if (m_pitchDegreeSpinBox->value() % 360 != 0) {
    double angle = glm::radians(double(m_pitchDegreeSpinBox->value()));
    m_camera.pitch(angle);
  }
}

void Z3DCameraControlWidget::createWidget()
{
  int minDegrees = -360;
  int maxDegrees = 360;

  auto vlo = new QVBoxLayout;
  QHBoxLayout* hlo = nullptr;

  hlo = new QHBoxLayout;
  QPushButton* azimuthButton = new QPushButton("Azimuth Camera", this);
  azimuthButton->setToolTip("Rotate the camera about the view up vector centered at the focal point. "
                              "The result is a horizontal rotation of the camera.");
  hlo->addWidget(azimuthButton);
  connect(azimuthButton, &QPushButton::clicked, this, &Z3DCameraControlWidget::azimuth);
  m_azimuthDegreeSpinBox = new QSpinBox(this);
  m_azimuthDegreeSpinBox->setMinimum(minDegrees);
  m_azimuthDegreeSpinBox->setMaximum(maxDegrees);
  m_azimuthDegreeSpinBox->setValue(90);
  m_azimuthDegreeSpinBox->setSuffix(" degrees");
  hlo->addWidget(m_azimuthDegreeSpinBox);
  vlo->addLayout(hlo);

  hlo = new QHBoxLayout;
  QPushButton* elevationButton = new QPushButton("Elevation Camera", this);
  elevationButton->setToolTip(
    "Rotate the camera about the cross product of the view up vector and the view vector (point at left in screen), "
      "using the focal point as the center of rotation. "
      "The result is a vertical rotation of the scene.");
  hlo->addWidget(elevationButton);
  connect(elevationButton, &QPushButton::clicked, this, &Z3DCameraControlWidget::elevation);
  m_elevationDegreeSpinBox = new QSpinBox(this);
  m_elevationDegreeSpinBox->setMinimum(minDegrees);
  m_elevationDegreeSpinBox->setMaximum(maxDegrees);
  m_elevationDegreeSpinBox->setValue(90);
  m_elevationDegreeSpinBox->setSuffix(" degrees");
  hlo->addWidget(m_elevationDegreeSpinBox);
  vlo->addLayout(hlo);

  hlo = new QHBoxLayout;
  QPushButton* rollButton = new QPushButton("Roll Camera", this);
  rollButton->setToolTip("Rotate the camera about the view vector. "
                           "This will spin the camera about its axis.");
  hlo->addWidget(rollButton);
  connect(rollButton, &QPushButton::clicked, this, &Z3DCameraControlWidget::roll);
  m_rollDegreeSpinBox = new QSpinBox(this);
  m_rollDegreeSpinBox->setMinimum(minDegrees);
  m_rollDegreeSpinBox->setMaximum(maxDegrees);
  m_rollDegreeSpinBox->setValue(90);
  m_rollDegreeSpinBox->setSuffix(" degrees");
  hlo->addWidget(m_rollDegreeSpinBox);
  vlo->addLayout(hlo);

  hlo = new QHBoxLayout;
  QPushButton* yawButton = new QPushButton("Yaw Camera", this);
  yawButton->setToolTip("Rotate the focal point about the view up vector, "
                          "using the camera's position as the center of rotation. "
                          "The result is a horizontal rotation of the scene.");
  hlo->addWidget(yawButton);
  connect(yawButton, &QPushButton::clicked, this, &Z3DCameraControlWidget::yaw);
  m_yawDegreeSpinBox = new QSpinBox(this);
  m_yawDegreeSpinBox->setMinimum(minDegrees);
  m_yawDegreeSpinBox->setMaximum(maxDegrees);
  m_yawDegreeSpinBox->setValue(90);
  m_yawDegreeSpinBox->setSuffix(" degrees");
  hlo->addWidget(m_yawDegreeSpinBox);
  vlo->addLayout(hlo);


  hlo = new QHBoxLayout;
  QPushButton* pitchButton = new QPushButton("Pitch Camera", this);
  pitchButton->setToolTip(
    "Rotate the focal point about the cross product of the view vector and the view up vector (point right in screen), "
      "using the camera's position as the center of rotation. "
      "The result is a vertical rotation of the camera.");
  hlo->addWidget(pitchButton);
  connect(pitchButton, &QPushButton::clicked, this, &Z3DCameraControlWidget::pitch);
  m_pitchDegreeSpinBox = new QSpinBox(this);
  m_pitchDegreeSpinBox->setMinimum(minDegrees);
  m_pitchDegreeSpinBox->setMaximum(maxDegrees);
  m_pitchDegreeSpinBox->setValue(90);
  m_pitchDegreeSpinBox->setSuffix(" degrees");
  hlo->addWidget(m_pitchDegreeSpinBox);
  vlo->addLayout(hlo);

  setLayout(vlo);
}

} // namespace nim
