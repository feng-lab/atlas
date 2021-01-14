#pragma once

#include "zanimation.h"

namespace nim {

class Z3DView;

class ZParameterAnimation;

class ZCameraParameterAnimation;

class Z3DAnimation : public ZAnimation
{
Q_OBJECT
public:
  explicit Z3DAnimation(ZDoc& doc, QObject* parent = nullptr);

  void bindView(Z3DView* v);

  void load(const QString& fn);

  void save(const QString& fn);

  [[nodiscard]] const ZCameraParameterAnimation* cameraParameterAnimation() const
  { return m_cameraParameterAnimation; }

  ZCameraParameterAnimation* cameraParameterAnimation()
  { return m_cameraParameterAnimation; }

protected:
  void bindGlobalParameters() override;

  void addGlobalKey(double time) override;

protected:
  // managed by parent class
  ZCameraParameterAnimation* m_cameraParameterAnimation;
};

} // namespace nim

