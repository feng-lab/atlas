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

  void load(const QString& fn);

  void save(const QString& fn);

  [[nodiscard]] bool is2DAnimation() const override
  {
    return true;
  }

protected:
  void bindGlobalParameters() override;

  void addGlobalKey(double time) override;

protected:
  // managed by parent class
  ZParameterAnimation* m_sliceAnimation;
  ZParameterAnimation* m_timeAnimation;
  ZParameterAnimation* m_viewStyleAnimation;
  ZParameterAnimation* m_viewportAnimation;
};

} // namespace nim
