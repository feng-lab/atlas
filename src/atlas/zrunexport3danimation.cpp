#include "zrunexport3danimation.h"
#include "zcommandlineflags.h"

#include "zlog.h"
#include "zdoc.h"
#include "z3drenderingengine.h"
#include "z3danimationdoc.h"
#include "zvideoencoder.h"
#include "zcpuinfo.h"
#include "zprocess.h"
#include "zstringutils.h"
#include <folly/ScopeGuard.h>
#include <folly/futures/Future.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <QCoreApplication>
#include <string>
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
ABSL_FLAG(bool,
          only_compress_video,
          false,
          "Only compress video from existing images in --output_image_folder_name, default is false. If true, "
          "--output_filename and --output_image_folder_name must be provided. --output_fps, "
          "--output_image_name_prefix, and --output_image_name_field_width should match the image exporting settings"
          "if the default values are not used.");
ABSL_FLAG(uint64_t,
          limit_memory_usage_in_gb_to,
          0,
          "Limit memory usage to a specific GB value. Only valid for values >= 32. Default: 0 (no limit)");
ABSL_FLAG(int32_t, output_tile_size, 1024, "Tile size for segmented rendering. Default: 1024");
ABSL_FLAG(int32_t, output_tile_border, 64, "Tile border size for segmented rendering. Default: 64");
ABSL_FLAG(int32_t, maximum_output_width, 15360, "Maximum possible output video width. Default: 15360");
ABSL_FLAG(int32_t, maximum_output_height, 8640, "Maximum possible output video height. Default: 8640");

ABSL_FLAG(std::vector<std::string>,
          use_gpu_devices,
          std::vector<std::string>{},
          "Comma-separated list of GPU device IDs to use (e.g., '0,1,2,3'). Linux only.");
ABSL_DECLARE_FLAG(uint32_t, use_gpu_device);

#if defined(__linux__)
ABSL_DECLARE_FLAG(bool, __use_EGL);
#endif

namespace nim {

int ZRunExport3DAnimation::run()
{
  LOG(INFO) << "Export 3D Animation Start";
  auto guard = folly::makeGuard([]() {
    LOG(INFO) << "Export 3D Animation End";
  });

  m_hasError = false;

  if (absl::GetFlag(FLAGS_limit_memory_usage_in_gb_to) >= 32) {
    ZCpuInfo::instance().setMemoryLimitInBytes(absl::GetFlag(FLAGS_limit_memory_usage_in_gb_to) * 1024 * 1024 * 1024);
  }

  auto outputFilename = QString::fromStdString(absl::GetFlag(FLAGS_output_filename)).trimmed();
  if (outputFilename.isEmpty()) {
    LOG(ERROR) << fmt::format("output file name ({}) is empty", absl::GetFlag(FLAGS_output_filename));
    return 1;
  }
  if (!outputFilename.endsWith(".mp4", Qt::CaseInsensitive)) {
    outputFilename += ".mp4";
  }

  auto outputImageFolderName = QString::fromStdString(absl::GetFlag(FLAGS_output_image_folder_name)).trimmed();

  if (absl::GetFlag(FLAGS_only_compress_video)) {
    ZVideoEncoder videoEncoder;
    connect(&videoEncoder, &ZVideoEncoder::error, this, &ZRunExport3DAnimation::logError);
    QString namePrefix = QString::fromStdString(absl::GetFlag(FLAGS_output_image_name_prefix));
    videoEncoder.encode(QDir(outputImageFolderName),
                        namePrefix,
                        absl::GetFlag(FLAGS_output_image_name_field_width),
                        absl::GetFlag(FLAGS_output_fps),
                        outputFilename);
    videoEncoder.waitForFinished(-1);
    LOG(INFO) << outputFilename << " saved";
    return m_hasError ? 1 : 0;
  }

  auto filename = QString::fromStdString(absl::GetFlag(FLAGS_filename));
  if (!QFile::exists(filename)) {
    LOG(ERROR) << fmt::format("input file ({}) does not exist", absl::GetFlag(FLAGS_filename));
    return 1;
  }

  if (absl::GetFlag(FLAGS_output_start_frame) == 0 && absl::GetFlag(FLAGS_output_end_frame) == -1) {
    if (absl::GetFlag(FLAGS_output_start_time) != 0.) {
      absl::SetFlag(&FLAGS_output_start_frame,
                    absl::GetFlag(FLAGS_output_start_time) * absl::GetFlag(FLAGS_output_fps));
    }
    if (absl::GetFlag(FLAGS_output_end_time) != -1.) {
      absl::SetFlag(&FLAGS_output_end_frame, absl::GetFlag(FLAGS_output_end_time) * absl::GetFlag(FLAGS_output_fps));
    }
  }

#if defined(__linux__)
  if (std::vector<std::string> gpuDevices = absl::GetFlag(FLAGS_use_gpu_devices); !gpuDevices.empty()) {
    std::vector<uint32_t> gpuList;
    for (const std::string& numStr : gpuDevices) {
      uint32_t v;
      if (!stringToValueNoThrow(numStr, v)) {
        LOG(ERROR) << fmt::format("invalid gpu device {}", numStr);
        return 1;
      }
      gpuList.push_back(v);
    }

    int totalEndFrame = absl::GetFlag(FLAGS_output_end_frame);
    if (totalEndFrame <= 0) {
      ZDoc doc;
      doc.animation3DDoc().setShowLoadIssueDialogs(false);
      QString errorMsg;
      if (size_t id = doc.animation3DDoc().loadFile(filename, errorMsg); id == 0) {
        LOG(ERROR) << "load animation file error: " << errorMsg;
        return 1;
      } else if (const QString& loadIssues = doc.animation3DDoc().animation(id).lastLoadIssues();
                 !loadIssues.isEmpty()) {
        LOG(ERROR) << "load animation file error: " << loadIssues;
        return 1;
      } else {
        totalEndFrame = std::max(
          1,
          static_cast<int>(std::ceil(doc.animation3DDoc().animation(id).duration() * absl::GetFlag(FLAGS_output_fps))));
      }
    }

    if (gpuList.size() == 1 || totalEndFrame <= absl::GetFlag(FLAGS_output_fps)) {
      absl::SetFlag(&FLAGS_use_gpu_device, gpuList[0]);
    } else {
      int nFramesForOneGPU =
        (totalEndFrame - absl::GetFlag(FLAGS_output_start_frame)) / static_cast<int>(gpuList.size());

      auto tempdir = std::make_shared<QTemporaryDir>();
      if (outputImageFolderName.isEmpty()) {
        outputImageFolderName = tempdir->path();
      }

      auto cpuExecutor = folly::getGlobalCPUExecutor();
      auto p = dynamic_cast<folly::CPUThreadPoolExecutor*>(cpuExecutor.get());
      CHECK(p);
      std::vector<folly::Future<folly::Unit>> gpuFutures;
      gpuFutures.reserve(gpuList.size());
      QString program = QCoreApplication::applicationFilePath();
      for (size_t idx = 0; idx < gpuList.size(); ++idx) {
        int startFrame = absl::GetFlag(FLAGS_output_start_frame) + int(idx) * nFramesForOneGPU;
        int endFrame = (idx + 1 == gpuList.size()) ? -1 : (startFrame + nFramesForOneGPU);
        gpuFutures.push_back(folly::via(cpuExecutor, [=]() {
          QStringList arguments;
          arguments << "--run_export_3d_animation"
                    << "--use_gpu_device" << QString::number(gpuList[idx]) << "--filename" << filename
                    << "--output_filename" << outputFilename << "--output_fps"
                    << QString::number(absl::GetFlag(FLAGS_output_fps)) << "--output_start_frame"
                    << QString::number(startFrame) << "--output_end_frame" << QString::number(endFrame)
                    << "--output_width" << QString::number(absl::GetFlag(FLAGS_output_width)) << "--output_height"
                    << QString::number(absl::GetFlag(FLAGS_output_height)) << "--output_image_folder_name"
                    << outputImageFolderName << "--skip_video_compression"
                    << "--limit_memory_usage_in_gb_to"
                    << QString::number(
                         absl::GetFlag(FLAGS_limit_memory_usage_in_gb_to) == 0
                           ? 0
                           : static_cast<int>(absl::GetFlag(FLAGS_limit_memory_usage_in_gb_to) / gpuList.size()))
                    << "--output_image_name_prefix"
                    << QString::fromStdString(absl::GetFlag(FLAGS_output_image_name_prefix))
                    << "--output_image_name_field_width"
                    << QString::number(absl::GetFlag(FLAGS_output_image_name_field_width)) << "--output_tile_size"
                    << QString::number(absl::GetFlag(FLAGS_output_tile_size)) << "--output_tile_border"
                    << QString::number(absl::GetFlag(FLAGS_output_tile_border));
          if (absl::GetFlag(FLAGS_overwrite)) {
            arguments << "--overwrite";
          }
          arguments << "-platform"
                    << "offscreen";

          ZProcess renderingProcess;
          renderingProcess.run(program, arguments);
          if (!renderingProcess.waitForStarted(-1)) {
            throw ZException("could not start rendering process");
          }
          if (!renderingProcess.waitForFinished(-1) || !renderingProcess.finishedWithoutError()) {
            throw ZException(fmt::format("rendering process error: {}", renderingProcess.processError()));
          }
          LOG(INFO) << "rendering process finished";
        }));
      }
      auto f = folly::collect(gpuFutures).via(cpuExecutor).then([=](auto&&) {
        LOG(INFO) << fmt::format("finish image rendering");
        if (!absl::GetFlag(FLAGS_skip_video_compression)) {
          QStringList arguments;
          arguments << "--run_export_3d_animation"
                    << "--only_compress_video"
                    << "--output_filename" << outputFilename << "--output_image_folder_name" << outputImageFolderName
                    << "--output_fps" << QString::number(absl::GetFlag(FLAGS_output_fps))
                    << "--output_image_name_prefix"
                    << QString::fromStdString(absl::GetFlag(FLAGS_output_image_name_prefix))
                    << "--output_image_name_field_width"
                    << QString::number(absl::GetFlag(FLAGS_output_image_name_field_width));
          if (absl::GetFlag(FLAGS_overwrite)) {
            arguments << "--overwrite";
          }
          arguments << "-platform"
                    << "offscreen";

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

      while (!f.wait(std::chrono::seconds(60)).isReady()) {
        auto poolStats = p->getPoolStats();
        LOG(INFO) << fmt::format("pending/total task count: {}/{}, active/idle thread count: {}/{}",
                                 poolStats.pendingTaskCount,
                                 poolStats.totalTaskCount,
                                 poolStats.activeThreadCount,
                                 poolStats.idleThreadCount);
      }
      try {
        std::move(f).get();
      }
      catch (const std::exception& e) {
        LOG(ERROR) << fmt::format("multi-GPU export failed: {}", e.what());
        return 1;
      }

      return 0;
    }
  }

  absl::SetFlag(&FLAGS___use_EGL, true);
#else
  if (!absl::GetFlag(FLAGS_use_gpu_devices).empty()) {
    LOG(ERROR) << "Flag --use_gpu_devices is Linux only";
  }
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
