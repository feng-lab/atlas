#pragma once

#include "zblockedautotracesession.h"

#include "zimg.h"
#include "zimgprocess.h"
#include "zneutubetraceconfig.h"

#include <folly/CancellationToken.h>

#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace nim {

// Blocked (core+halo) auto trace process with crash-safe resume checkpoints.
//
// This is the "large image" variant intended for ZImgPack-backed datasets (disk-cached or Neuroglancer),
// where assembling the full dense signal volume is not feasible.
class ZNeutubeBlockedAutoTraceProcess final : public ZImgProcess
{
public:
  struct RoiSignalResult
  {
    enum class Status
    {
      Ok,
      AllZero
    };

    Status status = Status::Ok;
    std::shared_ptr<ZImg> signal;

    [[nodiscard]] static RoiSignalResult ok(std::shared_ptr<ZImg> image)
    {
      CHECK(image != nullptr);
      return RoiSignalResult{.status = Status::Ok, .signal = std::move(image)};
    }

    [[nodiscard]] static RoiSignalResult allZero()
    {
      return RoiSignalResult{.status = Status::AllZero, .signal = nullptr};
    }
  };

  // Provides a single-channel, single-time signal ROI in the *tracing voxel coordinates*.
  //
  // Tracing voxel coordinates match the selected downsample ratio:
  // - ratio=[1,1,1] -> base voxel coordinates
  // - ratio=[2,2,1] -> half-resolution in XY, full in Z, etc.
  //
  // - (sx,sy,sz) is the ROI start voxel coordinate in the dataset.
  // - (w,h,d) is the ROI size in voxels.
  // - Returns `Status::AllZero` only when the ROI is known to be valid and entirely zero-valued.
  // - Backend unavailability/fetch failures must throw so the caller can retry instead of silently marking the block
  // visited.
  using RoiSignalProvider = std::function<
    RoiSignalResult(int64_t sx, int64_t sy, int64_t sz, int64_t w, int64_t h, int64_t d, folly::CancellationToken)>;

  ZNeutubeBlockedAutoTraceProcess() = default;

  void setInputImageSource(ZImgSource source)
  {
    m_inputImageSource = std::move(source);
  }

  void setInputImagePath(QString path)
  {
    m_inputImageSource = ZImgSource(std::move(path));
  }

  void setSignalInfo(ZImgInfo info)
  {
    m_signalInfo = std::move(info);
  }

  void setDatasetId(std::string datasetId)
  {
    m_datasetId = std::move(datasetId);
  }

  void setRoiSignalProvider(RoiSignalProvider provider)
  {
    m_roiProvider = std::move(provider);
  }

  void setSelectedChannelTime(size_t sc, size_t t)
  {
    m_selectedChannel = sc;
    m_selectedTime = t;
  }

  void setZToXYRatio(double zToXYRatio)
  {
    CHECK(std::isfinite(zToXYRatio));
    CHECK(zToXYRatio > 0.0);
    m_zToXYRatio = zToXYRatio;
  }

  void setSignalDownsampleRatio(std::array<size_t, 3> ratio)
  {
    CHECK(ratio[0] > 0);
    CHECK(ratio[1] > 0);
    CHECK(ratio[2] > 0);
    m_signalDownsampleRatio = ratio;
  }

  // Core block size in tracing voxels. The UI uses cubic blocks so this overrides X/Y/Z together.
  void setBlockCoreSize(int64_t voxels)
  {
    CHECK(voxels > 0);
    m_blockCoreX = voxels;
    m_blockCoreY = voxels;
    m_blockCoreZ = voxels;
  }

  // Core block size in tracing voxels, per axis.
  void setBlockCoreSizeXYZ(int64_t coreX, int64_t coreY, int64_t coreZ)
  {
    CHECK(coreX > 0);
    CHECK(coreY > 0);
    CHECK(coreZ > 0);
    m_blockCoreX = coreX;
    m_blockCoreY = coreY;
    m_blockCoreZ = coreZ;
  }

  // Halo/padding size in tracing voxels.
  void setBlockHalo(int64_t voxels)
  {
    CHECK(voxels >= 0);
    m_blockHalo = voxels;
  }

  void setTraceConfigPath(QString path)
  {
    m_traceConfigPath = std::move(path);
  }

  void setTraceConfig(json::object cfg)
  {
    m_traceConfig = std::move(cfg);
  }

  void clearTraceConfig()
  {
    m_traceConfig.reset();
  }

  void setTraceLevel(int level)
  {
    m_traceLevel = level;
  }

  void setAlgoConfigOverrides(const TraceConfig& cfg)
  {
    m_algoOverrides = cfg;
    m_haveAlgoOverrides = true;
  }

  void clearAlgoConfigOverrides()
  {
    m_haveAlgoOverrides = false;
  }

  void setDoResampleAfterTracing(bool enabled)
  {
    m_doResampleAfterTracing = enabled;
  }

  void setDocHasAnySwc(bool v)
  {
    m_docHasAnySwc = v;
  }

  void setOutputSwcPath(QString path)
  {
    m_outputSwcPath = std::move(path);
  }

  void setOutputSessionDir(QString path)
  {
    m_outputSessionDir = std::move(path);
  }

  [[nodiscard]] const QString& outputSessionDir() const
  {
    return m_outputSessionDir;
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
  [[nodiscard]] TraceConfig buildEffectiveTraceConfigOrThrow() const;

  [[nodiscard]] ZImgInfo effectiveSignalInfoOrThrow() const;

  [[nodiscard]] std::string effectiveDatasetIdOrThrow() const;

  [[nodiscard]] double effectiveZToXYRatioOrThrow(const ZImgInfo& signalInfo) const;

  void writeFinalSwcAtomicOrThrow(ZSwc& tree) const;

private:
  std::optional<ZImgSource> m_inputImageSource;
  ZImgInfo m_signalInfo;
  std::string m_datasetId;
  RoiSignalProvider m_roiProvider;

  size_t m_selectedChannel = 0;
  size_t m_selectedTime = 0;
  std::optional<double> m_zToXYRatio;

  std::array<size_t, 3> m_signalDownsampleRatio = {1, 1, 1};

  std::optional<int64_t> m_blockCoreX;
  std::optional<int64_t> m_blockCoreY;
  std::optional<int64_t> m_blockCoreZ;
  std::optional<int64_t> m_blockHalo;

  QString m_traceConfigPath;
  std::optional<json::object> m_traceConfig;
  int m_traceLevel = 0;

  bool m_haveAlgoOverrides = false;
  TraceConfig m_algoOverrides;

  bool m_doResampleAfterTracing = true;
  bool m_docHasAnySwc = false;

  QString m_outputSwcPath;
  QString m_outputSessionDir;

  bool m_hasResult = false;
};

} // namespace nim
