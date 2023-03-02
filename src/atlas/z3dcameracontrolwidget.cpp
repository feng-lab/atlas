#include "z3dcameracontrolwidget.h"

#include "z3drenderingengine.h"
#include "zdoc.h"
#include <QVBoxLayout>
#include <QPushButton>
#include <QSpinBox>

namespace nim {

Z3DCameraControlWidget::Z3DCameraControlWidget(Z3DCameraParameter& camera, Z3DRenderingEngine& engine, QWidget* parent)
  : QWidget(parent)
  , m_camera(camera)
  , m_view(engine)
{
  createWidget();
}

void Z3DCameraControlWidget::roll()
{
  if (m_rollDegreeSpinBox->value() % 360 != 0) {
    auto angle = glm::radians(float(m_rollDegreeSpinBox->value()));
    m_camera.roll(angle);
  }
}

void Z3DCameraControlWidget::azimuth()
{
  if (m_azimuthDegreeSpinBox->value() % 360 != 0) {
    auto angle = glm::radians(float(m_azimuthDegreeSpinBox->value()));
    m_camera.azimuth(angle);
  }
}

void Z3DCameraControlWidget::yaw()
{
  if (m_yawDegreeSpinBox->value() % 360 != 0) {
    auto angle = glm::radians(float(m_yawDegreeSpinBox->value()));
    m_camera.yaw(angle);
  }
}

void Z3DCameraControlWidget::elevation()
{
  if (m_elevationDegreeSpinBox->value() % 360 != 0) {
    auto angle = glm::radians(float(m_elevationDegreeSpinBox->value()));
    m_camera.elevation(angle);
  }
}

void Z3DCameraControlWidget::pitch()
{
  if (m_pitchDegreeSpinBox->value() % 360 != 0) {
    auto angle = glm::radians(float(m_pitchDegreeSpinBox->value()));
    m_camera.pitch(angle);
  }
}

void Z3DCameraControlWidget::focusOn()
{
  auto objIDs = m_view.doc().chooseObjsWithWidget(QString("Focuses on objects..."), this);
  if (!objIDs.empty()) {
    m_view.cameraFocusesOn(m_view.boundBoxOfObjsAfterClipping(objIDs));
  }
}

void Z3DCameraControlWidget::focusOnIgnoreClipping()
{
  auto objIDs = m_view.doc().chooseObjsWithWidget(QString("Focuses on objects (ignore clipping)..."), this);
  if (!objIDs.empty()) {
    m_view.cameraFocusesOn(m_view.boundBoxOfObjs(objIDs));
  }
}

void Z3DCameraControlWidget::pointsTo()
{
  auto objIDs = m_view.doc().chooseObjsWithWidget(QString("Camera points to objects..."), this);
  if (!objIDs.empty()) {
    m_view.cameraPointsTo(m_view.boundBoxOfObjsAfterClipping(objIDs));
  }
}

void Z3DCameraControlWidget::pointsToIgnoreClipping()
{
  auto objIDs = m_view.doc().chooseObjsWithWidget(QString("Camera points to objects (ignore clipping)..."), this);
  if (!objIDs.empty()) {
    m_view.cameraPointsTo(m_view.boundBoxOfObjs(objIDs));
  }
}

void Z3DCameraControlWidget::flipView()
{
  m_view.flipView();
}

void Z3DCameraControlWidget::setXYView()
{
  m_view.resetCameraAction()->trigger();
}

void Z3DCameraControlWidget::setXZView()
{
  m_view.setXZView();
}

void Z3DCameraControlWidget::setYZView()
{
  m_view.setYZView();
}

void Z3DCameraControlWidget::createWidget()
{
  auto minDegrees = -360;
  auto maxDegrees = 360;

  auto vlo = new QVBoxLayout;
  auto* hlo = new QHBoxLayout;
  auto* azimuthButton = new QPushButton("Azimuth Camera", this);
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
  auto* elevationButton = new QPushButton("Elevation Camera", this);
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
  auto* rollButton = new QPushButton("Roll Camera", this);
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
  auto* yawButton = new QPushButton("Yaw Camera", this);
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
  auto* pitchButton = new QPushButton("Pitch Camera", this);
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

  m_focusOnButton = new QPushButton("Focuses on Objects...", this);
  m_focusOnButton->setToolTip("Set the camera to focus on the selected objects");
  connect(m_focusOnButton, &QPushButton::clicked, this, &Z3DCameraControlWidget::focusOn);
  vlo->addWidget(m_focusOnButton);

  m_focusOnIgnoreClippingButton = new QPushButton("Focuses on Objects (ignore clipping)...", this);
  m_focusOnIgnoreClippingButton->setToolTip(
    "Set the camera to focus on the selected objects, clipping of selected objects are ignored.");
  connect(m_focusOnIgnoreClippingButton, &QPushButton::clicked, this, &Z3DCameraControlWidget::focusOnIgnoreClipping);
  vlo->addWidget(m_focusOnIgnoreClippingButton);

  m_moveCenterButton = new QPushButton("Camera Points to Objects...", this);
  m_moveCenterButton->setToolTip(
    "Move the center of camera to the center of selected objects, camera position will not be changed.");
  connect(m_moveCenterButton, &QPushButton::clicked, this, &Z3DCameraControlWidget::pointsTo);
  vlo->addWidget(m_moveCenterButton);

  m_moveCenterIgnoreClippingButton = new QPushButton("Camera Points to Objects (ignore clipping)...", this);
  m_moveCenterIgnoreClippingButton->setToolTip(
    "Move the center of camera to the center of selected objects ignoring their clippings, "
    "camera position will not be changed.");
  connect(m_moveCenterIgnoreClippingButton,
          &QPushButton::clicked,
          this,
          &Z3DCameraControlWidget::pointsToIgnoreClipping);
  vlo->addWidget(m_moveCenterIgnoreClippingButton);

  hlo = new QHBoxLayout;
  m_flipViewButton = new QPushButton("Flip", this);
  m_flipViewButton->setToolTip("Look from the oppsite side.");
  connect(m_flipViewButton, &QPushButton::clicked, this, &Z3DCameraControlWidget::flipView);
  hlo->addWidget(m_flipViewButton);
  m_setXYViewButton = new QPushButton("XY", this);
  m_setXYViewButton->setToolTip("reset to XY view.");
  connect(m_setXYViewButton, &QPushButton::clicked, this, &Z3DCameraControlWidget::setXYView);
  hlo->addWidget(m_setXYViewButton);
  m_setXZViewButton = new QPushButton("XZ", this);
  m_setXZViewButton->setToolTip("reset to XZ view.");
  connect(m_setXZViewButton, &QPushButton::clicked, this, &Z3DCameraControlWidget::setXZView);
  hlo->addWidget(m_setXZViewButton);
  m_setYZViewButton = new QPushButton("YZ", this);
  m_setYZViewButton->setToolTip("reset to YZ view.");
  connect(m_setYZViewButton, &QPushButton::clicked, this, &Z3DCameraControlWidget::setYZView);
  hlo->addWidget(m_setYZViewButton);
  vlo->addLayout(hlo);

  setLayout(vlo);
}

} // namespace nim
