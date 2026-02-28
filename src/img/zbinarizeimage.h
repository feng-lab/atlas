#pragma once

#include "zimgprocess.h"

namespace nim {

class ZBinarizeImage final : public ZImgProcess
{
public:
  enum class ThresholdMode
  {
    Manual,
    AutoLocmax,
  };

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

  void setThreshold(int thre)
  {
    m_threshold = thre;
  }

  void setThresholdMode(ThresholdMode mode)
  {
    m_thresholdMode = mode;
  }

  void setAutoThresholdRetryCount(int n)
  {
    m_autoThresholdRetryCount = n;
  }

protected:
  void doWork() override;

  void read(const json::object& jo) override;

  void write(json::object& jo) const override;

private:
  QString m_inputImagePath;
  QString m_outputImagePath;
  int m_channel = 0; // 0-based
  int m_threshold = 0;
  ThresholdMode m_thresholdMode = ThresholdMode::Manual;
  int m_autoThresholdRetryCount = 3;
};

} // namespace nim
