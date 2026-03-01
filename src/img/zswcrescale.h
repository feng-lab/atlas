#pragma once

#include "zimgprocess.h"

namespace nim {

struct SwcRescaleSettings
{
  // Applied before scaling: pos = pos + preTranslate
  double preTranslateX = 0.0;
  double preTranslateY = 0.0;
  double preTranslateZ = 0.0;

  double scaleX = 1.0;
  double scaleY = 1.0;
  double scaleZ = 1.0;

  // Applied after scaling: pos = pos + postTranslate
  double postTranslateX = 0.0;
  double postTranslateY = 0.0;
  double postTranslateZ = 0.0;

  // If true, scales node radii by sqrt(scaleX * scaleY), matching neuTu's
  // `ZSwcTree::rescale(..., changingRadius=true)` behavior.
  bool scaleRadius = true;
};

class ZSwcRescale : public ZImgProcess
{
public:
  void setInputSwcFilename(const QString& fn)
  {
    m_inputSwcFilename = fn;
  }

  void setOutputSwcFilename(const QString& fn)
  {
    m_outputSwcFilename = fn;
  }

  void setSettings(const SwcRescaleSettings& settings)
  {
    m_settings = settings;
  }

protected:
  void doWork() override;

  void read(const json::object& jo) override;

  void write(json::object& jo) const override;

private:
  QString m_inputSwcFilename;
  QString m_outputSwcFilename;
  SwcRescaleSettings m_settings;
};

} // namespace nim
