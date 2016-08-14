#pragma once

#include "zanimation.h"

namespace nim {

class ZView;

class ZParameterAnimation;

class Z2DAnimation : public ZAnimation
{
Q_OBJECT
public:
  explicit Z2DAnimation(ZDoc& doc, QObject* parent = nullptr);

  void bindView(ZView* v);

  void load(const QString& filename);

  void save(const QString& filename);

  virtual bool is2DAnimation() const override
  { return true; }

protected:
  virtual void bindGlobalParameters() override;

  virtual void addGlobalKey(double time) override;

protected:
  // managed by parent class
  ZParameterAnimation* m_sliceAnimation;
  ZParameterAnimation* m_timeAnimation;
  ZParameterAnimation* m_mipAnimation;
  ZParameterAnimation* m_viewportAnimation;
};

} // namespace nim


