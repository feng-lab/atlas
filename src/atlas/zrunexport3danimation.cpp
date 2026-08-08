#include "zrunexport3danimation.h"
#include "zcommandlineflags.h"

#include "zlog.h"
#include "zdoc.h"
#include "z3drenderingengine.h"
#include "z3danimationdoc.h"
#include "zvideoencoder.h"
#include "zcpuinfo.h"
#include "zexception.h"
#include "zprocess.h"
#include "zstringutils.h"
#include <folly/ScopeGuard.h>
#include <folly/futures/Future.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

ABSL_FLAG(bool, run_export_3d_animation, false, "Enable exporting 3D animation via command line");
ABSL_FLAG(std::string,
          filename,
          "",
          "Input file name (.animation3d for animation export, .scene for scene export depending on the selected "
          "headless export mode)");
ABSL_FLAG(std::string, output_filename, "", "Output file name (video for animation export, image for scene export)");
ABSL_FLAG(int32_t, output_fps, 30, "Output video frame rate (FPS). Default: 30");
ABSL_FLAG(double,
          output_start_time,
          0.,
          "(deprecated, use output_start_frame) Output video start time in seconds. Default: 0.0");
ABSL_FLAG(double,
          output_end_time,
          -1.,
          "(deprecated, use output_end_frame) Output video end time in seconds. Default: -1.0 (end of animation)");
ABSL_FLAG(int32_t, output_start_frame, 0, "Output video start frame. Default: 0");
ABSL_FLAG(int32_t, output_end_frame, -1, "Output video end frame. Default: -1 (end of animation)");
ABSL_FLAG(int32_t, output_width, 3840, "Export output width. Default: 3840");
ABSL_FLAG(int32_t, output_height, 2160, "Export output height. Default: 2160");
ABSL_FLAG(bool, overwrite, false, "Overwrite an existing output file. Default: false");
ABSL_FLAG(std::string, output_image_folder_name, "", "Folder for output images. Uses temp folder if empty");
ABSL_FLAG(bool, skip_video_compression, false, "Skip video compression. If true, specify --output_image_folder_name");
ABSL_DECLARE_FLAG(std::string, output_image_name_prefix);
ABSL_DECLARE_FLAG(int32_t, output_image_name_field_width);
ABSL_DECLARE_FLAG(nim::RenderBackend, atlas_default_render_backend);
ABSL_DECLARE_FLAG(int32_t, atlas_vk_device_index);
ABSL_FLAG(bool,
          only_compress_video,
          false,
          "Only compress video from existing images in --output_image_folder_name, default is false. If true, "
          "--output_filename and --output_image_folder_name must be provided. --output_fps, "
          "--output_image_name_prefix, and --output_image_name_field_width should match the image exporting settings "
          "if the default values are not used. --output_start_frame and --output_end_frame select the half-open input "
          "frame range; an end of -1 encodes the remaining contiguous sequence.");
ABSL_FLAG(uint64_t,
          limit_memory_usage_in_gb_to,
          0,
          "Set the Atlas host-memory sizing budget in GiB. Multi-process animation divides an explicit total among "
          "active workers. Default: 0 (use detected memory in each process)");
ABSL_FLAG(int32_t, output_tile_size, 1024, "Tile size for segmented rendering. Default: 1024");
ABSL_FLAG(int32_t, output_tile_border, 64, "Tile border size for segmented rendering. Default: 64");
ABSL_FLAG(int32_t, maximum_output_width, 15360, "Maximum possible output video width. Default: 15360");
ABSL_FLAG(int32_t, maximum_output_height, 8640, "Maximum possible output video height. Default: 8640");

ABSL_FLAG(std::vector<std::string>,
          use_gpu_devices,
          std::vector<std::string>{},
          "Comma-separated backend device indices to use (e.g., '0,1,2,3'). OpenGL values select EGL device IDs; "
          "Vulkan values select preference-sorted Vulkan device indices. Vulkan animation workers are supported on "
          "all platforms; explicit OpenGL devices require Linux EGL.");
ABSL_DECLARE_FLAG(uint32_t, use_gpu_device);

#if defined(__linux__)
ABSL_DECLARE_FLAG(bool, __use_EGL);
#endif

namespace nim {

namespace {

constexpr uint64_t kBytesPerGiB = 1024u * 1024u * 1024u;

bool validateFrameSequence(const QDir& directory,
                           const QString& namePrefix,
                           int fieldWidth,
                           int startFrame,
                           int endFrame,
                           QString& error)
{
  CHECK_GT(fieldWidth, 0);
  CHECK_GE(startFrame, 0);
  CHECK_GT(endFrame, startFrame);

  for (int frame = startFrame; frame < endFrame; ++frame) {
    const QString filename = QString("%1%2.png").arg(namePrefix).arg(frame, fieldWidth, 10, QChar('0'));
    const QFileInfo frameInfo(directory.filePath(filename));
    if (!frameInfo.isFile() || !frameInfo.isReadable()) {
      error = QString("Animation frame %1 does not exist or is not readable: %2")
                .arg(frame)
                .arg(frameInfo.absoluteFilePath());
      return false;
    }
  }
  return true;
}

bool prepareVideoOutput(const QString& outputFilename, bool overwriteExisting, QString& error)
{
  const QFileInfo outputInfo(outputFilename);
  QDir outputDir = outputInfo.absoluteDir();
  if (!outputDir.exists() && !outputDir.mkpath(".")) {
    error = QString("Can not create folder %1").arg(outputDir.absolutePath());
    return false;
  }

  if (!outputInfo.exists()) {
    return true;
  }
  if (!overwriteExisting) {
    error = QString("File %1 already exists").arg(outputInfo.absoluteFilePath());
    return false;
  }
  if (!QFile::remove(outputInfo.absoluteFilePath())) {
    error = QString("Can not replace existed file %1").arg(outputInfo.absoluteFilePath());
    return false;
  }
  return true;
}

} // namespace

std::vector<ZRunExport3DAnimation::FrameRange>
ZRunExport3DAnimation::splitFrameRange(int startFrame, int endFrame, size_t maxWorkerCount)
{
  CHECK_GE(startFrame, 0);
  CHECK_LT(startFrame, endFrame);
  CHECK_GT(maxWorkerCount, 0u);

  const size_t frameCount = static_cast<size_t>(static_cast<int64_t>(endFrame) - startFrame);
  const size_t workerCount = std::min(maxWorkerCount, frameCount);
  const size_t baseFrameCount = frameCount / workerCount;
  const size_t remainder = frameCount % workerCount;

  std::vector<FrameRange> ranges;
  ranges.reserve(workerCount);
  int rangeStart = startFrame;
  for (size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
    const size_t rangeFrameCount = baseFrameCount + (workerIndex < remainder ? 1u : 0u);
    CHECK_GT(rangeFrameCount, 0u);
    const int rangeEnd = rangeStart + static_cast<int>(rangeFrameCount);
    ranges.emplace_back(rangeStart, rangeEnd);
    rangeStart = rangeEnd;
  }
  CHECK_EQ(rangeStart, endFrame);
  return ranges;
}

int ZRunExport3DAnimation::run(const QStringList& childQpaPlatformArguments)
{
  LOG(INFO) << "Export 3D Animation Start";
  auto guard = folly::makeGuard([]() {
    LOG(INFO) << "Export 3D Animation End";
  });

  m_hasError = false;

  auto outputFilename = QString::fromStdString(absl::GetFlag(FLAGS_output_filename)).trimmed();
  if (outputFilename.isEmpty()) {
    LOG(ERROR) << fmt::format("output file name ({}) is empty", absl::GetFlag(FLAGS_output_filename));
    return 1;
  }
  if (!outputFilename.endsWith(".mp4", Qt::CaseInsensitive)) {
    outputFilename += ".mp4";
  }

  auto outputImageFolderName = QString::fromStdString(absl::GetFlag(FLAGS_output_image_folder_name)).trimmed();
  const int outputFps = absl::GetFlag(FLAGS_output_fps);
  if (outputFps <= 0) {
    LOG(ERROR) << fmt::format("output frame rate ({}) must be positive", outputFps);
    return 1;
  }

  if (absl::GetFlag(FLAGS_output_start_frame) == 0 && absl::GetFlag(FLAGS_output_end_frame) == -1) {
    if (absl::GetFlag(FLAGS_output_start_time) != 0.) {
      absl::SetFlag(&FLAGS_output_start_frame, absl::GetFlag(FLAGS_output_start_time) * outputFps);
    }
    if (absl::GetFlag(FLAGS_output_end_time) != -1.) {
      absl::SetFlag(&FLAGS_output_end_frame, absl::GetFlag(FLAGS_output_end_time) * outputFps);
    }
  }

  if (absl::GetFlag(FLAGS_only_compress_video)) {
    if (outputImageFolderName.isEmpty()) {
      LOG(ERROR) << "Image output folder must be specified when compressing existing animation frames";
      return 1;
    }
    const int outputImageFieldWidth = absl::GetFlag(FLAGS_output_image_name_field_width);
    if (outputImageFieldWidth <= 0) {
      LOG(ERROR) << fmt::format("output image name field width ({}) must be positive", outputImageFieldWidth);
      return 1;
    }
    const QString namePrefix = QString::fromStdString(absl::GetFlag(FLAGS_output_image_name_prefix));
    const int startFrame = absl::GetFlag(FLAGS_output_start_frame);
    const int endFrame = absl::GetFlag(FLAGS_output_end_frame);
    if (startFrame < 0) {
      LOG(ERROR) << fmt::format("Video start frame {} is not correct", startFrame);
      return 1;
    }
    std::optional<size_t> frameCount;
    if (endFrame >= 0) {
      if (endFrame <= startFrame) {
        LOG(ERROR) << fmt::format("Video end frame {} is not correct", endFrame);
        return 1;
      }
      frameCount = static_cast<size_t>(static_cast<int64_t>(endFrame) - startFrame);
      QString frameError;
      if (!validateFrameSequence(QDir(outputImageFolderName),
                                 namePrefix,
                                 outputImageFieldWidth,
                                 startFrame,
                                 endFrame,
                                 frameError)) {
        LOG(ERROR) << frameError;
        return 1;
      }
    }
    QString outputError;
    if (!prepareVideoOutput(outputFilename, absl::GetFlag(FLAGS_overwrite), outputError)) {
      LOG(ERROR) << outputError;
      return 1;
    }
    ZVideoEncoder videoEncoder;
    connect(&videoEncoder, &ZVideoEncoder::error, this, &ZRunExport3DAnimation::logError);
    videoEncoder.encode(QDir(outputImageFolderName),
                        namePrefix,
                        outputImageFieldWidth,
                        outputFps,
                        startFrame,
                        frameCount,
                        outputFilename);
    videoEncoder.waitForFinished(-1);
    LOG(INFO) << outputFilename << " saved";
    return m_hasError ? 1 : 0;
  }

  const uint64_t configuredMemoryLimitGiB = absl::GetFlag(FLAGS_limit_memory_usage_in_gb_to);
  if (configuredMemoryLimitGiB > std::numeric_limits<uint64_t>::max() / kBytesPerGiB) {
    LOG(ERROR) << "memory limit is too large";
    return 1;
  }
  if (configuredMemoryLimitGiB > 0u) {
    ZCpuInfo::instance().setMemoryLimitInBytes(configuredMemoryLimitGiB * kBytesPerGiB);
  }

  auto filename = QString::fromStdString(absl::GetFlag(FLAGS_filename));
  if (!QFile::exists(filename)) {
    LOG(ERROR) << fmt::format("input file ({}) does not exist", absl::GetFlag(FLAGS_filename));
    return 1;
  }
  if (absl::GetFlag(FLAGS_skip_video_compression) && outputImageFolderName.isEmpty()) {
    LOG(ERROR) << "Image output folder must be specified when video compression is skipped";
    return 1;
  }

  const RenderBackend requestedBackend = absl::GetFlag(FLAGS_atlas_default_render_backend);
  if (const std::vector<std::string> gpuDevices = absl::GetFlag(FLAGS_use_gpu_devices); !gpuDevices.empty()) {
#if !defined(__linux__)
    if (requestedBackend != RenderBackend::Vulkan) {
      LOG(ERROR) << "Explicit OpenGL devices for animation export require Linux EGL";
      return 1;
    }
#endif

    std::vector<uint32_t> gpuList;
    gpuList.reserve(gpuDevices.size());
    for (const std::string& numStr : gpuDevices) {
      uint32_t deviceIndex = 0;
      if (!stringToValueNoThrow(numStr, deviceIndex)) {
        LOG(ERROR) << fmt::format("invalid gpu device {}", numStr);
        return 1;
      }
      if (requestedBackend == RenderBackend::Vulkan &&
          deviceIndex > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        LOG(ERROR) << fmt::format("Vulkan device index {} exceeds the supported command-line range", deviceIndex);
        return 1;
      }
      gpuList.push_back(deviceIndex);
    }
    CHECK(!gpuList.empty());

    auto selectDirectDevice = [requestedBackend](uint32_t deviceIndex) {
      if (requestedBackend == RenderBackend::Vulkan) {
        absl::SetFlag(&FLAGS_atlas_vk_device_index, static_cast<int32_t>(deviceIndex));
      } else {
        absl::SetFlag(&FLAGS_use_gpu_device, deviceIndex);
      }
    };

    if (gpuList.size() == 1u) {
      selectDirectDevice(gpuList.front());
    } else {
      ZDoc doc;
      doc.animation3DDoc().setShowLoadIssueDialogs(false);
      QString errorMsg;
      const size_t animationId = doc.animation3DDoc().loadFile(filename, errorMsg);
      if (animationId == 0) {
        LOG(ERROR) << "load animation file error: " << errorMsg;
        return 1;
      }
      if (const QString& loadIssues = doc.animation3DDoc().animation(animationId).lastLoadIssues();
          !loadIssues.isEmpty()) {
        LOG(ERROR) << "load animation file error: " << loadIssues;
        return 1;
      }

      const int totalFrameCount =
        std::max(1, static_cast<int>(std::ceil(doc.animation3DDoc().animation(animationId).duration() * outputFps)));
      const int startFrame = absl::GetFlag(FLAGS_output_start_frame);
      if (startFrame < 0 || startFrame >= totalFrameCount) {
        LOG(ERROR) << fmt::format("Video start frame {} is not correct", startFrame);
        return 1;
      }
      const int requestedEndFrame = absl::GetFlag(FLAGS_output_end_frame);
      const int endFrame =
        requestedEndFrame < 0 || requestedEndFrame > totalFrameCount ? totalFrameCount : requestedEndFrame;
      if (endFrame <= startFrame) {
        LOG(ERROR) << fmt::format("Video end frame {} is not correct", requestedEndFrame);
        return 1;
      }

      const std::vector<FrameRange> frameRanges = splitFrameRange(startFrame, endFrame, gpuList.size());
      CHECK(!frameRanges.empty());
      if (frameRanges.size() == 1u) {
        selectDirectDevice(gpuList.front());
      } else {
        const uint64_t workerMemoryLimitGiB =
          configuredMemoryLimitGiB == 0u ? 0u : configuredMemoryLimitGiB / frameRanges.size();
        if (configuredMemoryLimitGiB > 0u && workerMemoryLimitGiB == 0u) {
          LOG(ERROR) << fmt::format("host-memory budget {} GiB is too small for {} active animation workers; the "
                                    "whole-GiB option must provide at least 1 GiB per worker",
                                    configuredMemoryLimitGiB,
                                    frameRanges.size());
          return 1;
        }
        if (!absl::GetFlag(FLAGS_skip_video_compression)) {
          QString outputError;
          if (!prepareVideoOutput(outputFilename, absl::GetFlag(FLAGS_overwrite), outputError)) {
            LOG(ERROR) << outputError;
            return 1;
          }
        }

        std::optional<QTemporaryDir> temporaryFrameDirectory;
        if (outputImageFolderName.isEmpty()) {
          temporaryFrameDirectory.emplace();
          if (!temporaryFrameDirectory->isValid()) {
            LOG(ERROR) << "Could not create a temporary animation export directory";
            return 1;
          }
          outputImageFolderName = temporaryFrameDirectory->path();
        }

        auto cpuExecutor = folly::getGlobalCPUExecutor();
        std::vector<folly::Future<folly::Unit>> gpuFutures;
        gpuFutures.reserve(frameRanges.size());
        const QString program = QCoreApplication::applicationFilePath();
        const int requiredImageFieldWidth = static_cast<int>(QString::number(totalFrameCount - 1).size());
        const int outputImageFieldWidth =
          std::max(absl::GetFlag(FLAGS_output_image_name_field_width), requiredImageFieldWidth);
        const bool overwrite = absl::GetFlag(FLAGS_overwrite);
        const QStringList qpaPlatformArguments = childQpaPlatformArguments;
        for (size_t workerIndex = 0; workerIndex < frameRanges.size(); ++workerIndex) {
          const auto [workerStartFrame, workerEndFrame] = frameRanges[workerIndex];
          const uint32_t deviceIndex = gpuList[workerIndex];
          gpuFutures.push_back(folly::via(cpuExecutor, [=]() {
            QStringList arguments;
            arguments << "--run_export_3d_animation"
                      << "--use_gpu_devices="
                      << "--only_compress_video=false"
                      << "--filename" << filename << "--atlas_default_render_backend"
                      << (requestedBackend == RenderBackend::Vulkan ? "vulkan" : "opengl") << "--output_filename"
                      << outputFilename << "--output_fps" << QString::number(outputFps) << "--output_start_frame"
                      << QString::number(workerStartFrame) << "--output_end_frame" << QString::number(workerEndFrame)
                      << "--output_width" << QString::number(absl::GetFlag(FLAGS_output_width)) << "--output_height"
                      << QString::number(absl::GetFlag(FLAGS_output_height)) << "--output_image_folder_name"
                      << outputImageFolderName << "--skip_video_compression"
                      << "--limit_memory_usage_in_gb_to"
                      << QString::number(static_cast<qulonglong>(workerMemoryLimitGiB)) << "--output_image_name_prefix"
                      << QString::fromStdString(absl::GetFlag(FLAGS_output_image_name_prefix))
                      << "--output_image_name_field_width"
                      << QString::number(absl::GetFlag(FLAGS_output_image_name_field_width)) << "--output_tile_size"
                      << QString::number(absl::GetFlag(FLAGS_output_tile_size)) << "--output_tile_border"
                      << QString::number(absl::GetFlag(FLAGS_output_tile_border)) << "--maximum_output_width"
                      << QString::number(absl::GetFlag(FLAGS_maximum_output_width)) << "--maximum_output_height"
                      << QString::number(absl::GetFlag(FLAGS_maximum_output_height));
            if (requestedBackend == RenderBackend::Vulkan) {
              arguments << "--atlas_vk_device_index" << QString::number(deviceIndex);
            } else {
              arguments << "--use_gpu_device" << QString::number(deviceIndex);
            }
            arguments << qpaPlatformArguments;

            ZProcess renderingProcess;
            renderingProcess.run(program, arguments);
            if (!renderingProcess.waitForStarted(-1)) {
              throw ZException("could not start rendering process");
            }
            if (!renderingProcess.waitForFinished(-1) || !renderingProcess.finishedWithoutError()) {
              throw ZException(fmt::format("rendering process error: {}", renderingProcess.processError()));
            }
            LOG(INFO) << fmt::format("rendering process finished for frames [{}, {}) on device {}",
                                     workerStartFrame,
                                     workerEndFrame,
                                     deviceIndex);
          }));
        }
        auto f = folly::collectAll(gpuFutures).via(cpuExecutor).thenValue([=](auto&& workerResults) {
          for (auto& result : workerResults) {
            result.throwUnlessValue();
          }
          LOG(INFO) << "finish image rendering";
          if (!absl::GetFlag(FLAGS_skip_video_compression)) {
            QStringList arguments;
            arguments << "--run_export_3d_animation"
                      << "--only_compress_video" << QString("--overwrite=%1").arg(overwrite ? "true" : "false")
                      << "--output_filename" << outputFilename << "--output_image_folder_name" << outputImageFolderName
                      << "--output_fps" << QString::number(outputFps) << "--output_start_frame"
                      << QString::number(startFrame) << "--output_end_frame" << QString::number(endFrame)
                      << "--output_image_name_prefix"
                      << QString::fromStdString(absl::GetFlag(FLAGS_output_image_name_prefix))
                      << "--output_image_name_field_width" << QString::number(outputImageFieldWidth);
            arguments << qpaPlatformArguments;

            ZProcess videoEncoderProcess;
            videoEncoderProcess.run(program, arguments);
            if (!videoEncoderProcess.waitForStarted(-1)) {
              throw ZException("could not start video encoding process");
            }
            if (!videoEncoderProcess.waitForFinished(-1) || !videoEncoderProcess.finishedWithoutError()) {
              throw ZException(fmt::format("video encoding process error: {}", videoEncoderProcess.processError()));
            }
            LOG(INFO) << outputFilename << " saved";
          }
        });

        try {
          std::move(f).get();
        }
        catch (const std::exception& e) {
          LOG(ERROR) << fmt::format("multi-process animation export failed after all workers stopped: {}", e.what());
          return 1;
        }

        return 0;
      }
    }
  }

#if defined(__linux__)
  // Headless animation engines do not need the GUI-created GL helper surface.
  // OpenGL creates its device-selecting context through EGL; Vulkan creates no
  // GL context at all.
  absl::SetFlag(&FLAGS___use_EGL, true);
#endif

  ZDoc doc;
  doc.animation3DDoc().setShowLoadIssueDialogs(false);
  Z3DRenderingEngine engine(doc);
  m_engine = &engine;
  auto resetEngineGuard = folly::makeGuard([this]() {
    m_engine = nullptr;
  });
  connect(&engine, &Z3DRenderingEngine::renderingError, this, &ZRunExport3DAnimation::logError);
  engine.init();

  QString errorMsg;
  size_t id;
  if (id = doc.animation3DDoc().loadFile(filename, errorMsg); id == 0) {
    LOG(ERROR) << "load animation file error: " << errorMsg;
    return 1;
  }
  if (const QString& loadIssues = doc.animation3DDoc().animation(id).lastLoadIssues(); !loadIssues.isEmpty()) {
    LOG(ERROR) << "load animation file error: " << loadIssues;
    return 1;
  }

  doc.animation3DDoc().bindView(&engine);
  if (m_hasError) {
    return 1;
  }

  engine.exportFixedSize3DAnimation(&doc.animation3DDoc().animation(id),
                                    outputFilename,
                                    absl::GetFlag(FLAGS_output_fps),
                                    absl::GetFlag(FLAGS_output_start_frame),
                                    absl::GetFlag(FLAGS_output_end_frame),
                                    absl::GetFlag(FLAGS_output_width),
                                    absl::GetFlag(FLAGS_output_height),
                                    absl::GetFlag(FLAGS_overwrite),
                                    Z3DScreenShotType::MonoView,
                                    outputImageFolderName.isEmpty() ? nullptr : &outputImageFolderName,
                                    absl::GetFlag(FLAGS_skip_video_compression),
                                    absl::GetFlag(FLAGS_output_tile_size),
                                    absl::GetFlag(FLAGS_output_tile_border));

  return m_hasError ? 1 : 0;
}

void ZRunExport3DAnimation::logError(const QString& err)
{
  LOG(ERROR) << err;
  m_hasError = true;
  if (m_engine != nullptr) {
    m_engine->cancelCapture();
    m_engine->cancelLongRendering();
  }
}

} // namespace nim
