#ifndef Z3DANIMATION_H
#define Z3DANIMATION_H

#include "zanimation.h"
#include <map>

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

  void load(const QString& filename);

  void save(const QString& filename);

  const ZCameraParameterAnimation* cameraParameterAnimation() const
  { return m_cameraParameterAnimation; }

  ZCameraParameterAnimation* cameraParameterAnimation()
  { return m_cameraParameterAnimation; }

protected:
  virtual void bindGlobalParameters() override;

  virtual void addGlobalKey(double time) override;

protected:
  // managed by parent class
  ZCameraParameterAnimation* m_cameraParameterAnimation;
};

} // namespace nim

#endif // Z3DANIMATION_H
