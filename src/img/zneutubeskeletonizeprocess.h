#pragma once

#include "zimg.h"
#include "zimgprocess.h"

#include <array>
#include <optional>

namespace nim {

class ZSwc;

class ZNeutubeSkeletonizeProcess final : public ZImgProcess
{
public:
  void setInputImageSource(ZImgSource source)
  {
    m_inputImageSource = std::move(source);
  }

  void setInputImagePath(QString path)
  {
    m_inputImageSource = ZImgSource(std::move(path));
  }

  void setSkeletonizeConfigPath(QString path)
  {
    m_skeletonizeConfigPath = std::move(path);
  }

  void setSkeletonizeConfig(json::object cfg)
  {
    m_skeletonizeConfig = std::move(cfg);
  }

  void setDownsampleIntervalOverride(const std::optional<std::array<int, 3>>& v)
  {
    m_downsampleIntervalOverride = v;
  }

  void setVerbose(bool v)
  {
    m_verbose = v;
  }

  void setOutputSwcPath(QString path)
  {
    m_outputSwcPath = std::move(path);
  }

  [[nodiscard]] const QString& outputSwcPath() const
  {
    return m_outputSwcPath;
  }

  [[nodiscard]] bool hasResult() const
  {
    return m_hasResult;
  }

protected:
  void doWork() override;

  void read(const json::object& jo) override;

  void write(json::object& jo) const override;

private:
  void writeSwcAtomicOrThrow(ZSwc& tree) const;

private:
  std::optional<ZImgSource> m_inputImageSource;
  QString m_skeletonizeConfigPath;
  std::optional<json::object> m_skeletonizeConfig;
  std::optional<std::array<int, 3>> m_downsampleIntervalOverride;
  bool m_verbose = false;

  QString m_outputSwcPath;
  bool m_hasResult = false;
};

} // namespace nim
