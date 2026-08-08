#pragma once

#include "zanimation.h"

namespace nim {

class Z3DRenderingEngine;

class ZParameterAnimation;

class ZCameraParameterAnimation;

class Z3DAnimation : public ZAnimation
{
  Q_OBJECT

public:
  explicit Z3DAnimation(ZDoc& doc, QObject* parent = nullptr);

  void bindView(Z3DRenderingEngine* v);

  void load(const QString& fn, bool showLoadIssuesDialog = true);

  // Read only the timeline duration needed to divide an export into frame ranges.
  // Referenced scene data remains owned and loaded by each rendering process.
  [[nodiscard]] static double readDurationFromFile(const QString& fn);

  void save(const QString& fn);

  [[nodiscard]] const ZCameraParameterAnimation* cameraParameterAnimation() const
  {
    return m_cameraParameterAnimation;
  }

  ZCameraParameterAnimation* cameraParameterAnimation()
  {
    return m_cameraParameterAnimation;
  }

protected:
  void bindGlobalParameters() override;

  void addGlobalKey(double time) override;

protected:
  // managed by parent class
  ZCameraParameterAnimation* m_cameraParameterAnimation;
};

} // namespace nim
