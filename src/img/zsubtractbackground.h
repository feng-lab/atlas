#pragma once

#include "zimgprocess.h"

namespace nim {

class ZSubtractBackground final : public ZImgProcess
{
public:
  void setInputImagePath(const QString& path)
  {
    m_inputImagePath = path;
  }

  void setOutputImagePath(const QString& path)
  {
    m_outputImagePath = path;
  }

  void setChannel(int c)
  {
    m_channel = c;
  }

  void setMinForegroundRatio(double minFr)
  {
    m_minForegroundRatio = minFr;
  }

  void setMaxIterations(int maxIter)
  {
    m_maxIterations = maxIter;
  }

protected:
  void doWork() override;

  void read(const json::object& jo) override;

  void write(json::object& jo) const override;

private:
  QString m_inputImagePath;
  QString m_outputImagePath;
  int m_channel = 0; // 0-based
  double m_minForegroundRatio = 0.5;
  int m_maxIterations = 3;
};

} // namespace nim
