#pragma once

#include "zimgprocess.h"

namespace nim {

class ZEnhanceLine final : public ZImgProcess
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

  void setSigma(double sigma)
  {
    m_sigma = sigma;
  }

protected:
  void doWork() override;

  void read(const json::object& jo) override;

  void write(json::object& jo) const override;

private:
  QString m_inputImagePath;
  QString m_outputImagePath;
  int m_channel = 0; // 0-based
  double m_sigma = 1.0;
};

} // namespace nim
