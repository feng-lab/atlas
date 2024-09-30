#include "zrunexport3danimation.h"

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

DEFINE_bool(run_export_3d_animation, false, "Enable exporting 3D animation via command line");
DEFINE_string(filename, "", "Input file name (.animation3d format)");
DEFINE_string(output_filename, "", "Output video file name");
DEFINE_int32(output_fps, 30, "Output video frame rate (FPS). Default: 30");
DEFINE_double(output_start_time,
              0.,
              "(deprecated, use output_start_frame) Output video start time in seconds. Default: 0.0");
DEFINE_double(output_end_time,
              -1.,
              "(deprecated, use output_end_frame) Output video end time in seconds. Default: -1.0 (end of animation)");
DEFINE_int32(output_start_frame, 0, "Output video start frame. Default: 0");
DEFINE_int32(output_end_frame, -1, "Output video end frame. Default: -1 (end of animation)");
DEFINE_int32(output_width, 3840, "Output video width. Default: 3840");
DEFINE_int32(output_height, 2160, "Output video height. Default: 2160");
DEFINE_bool(overwrite, false, "Overwrite existing output file. Default: false");
DEFINE_string(output_image_folder_name, "", "Folder for output images. Uses temp folder if empty");
DEFINE_bool(skip_video_compression, false, "Skip video compression. If true, specify --output_image_folder_name");
DECLARE_string(output_image_name_prefix);
DECLARE_int32(output_image_name_field_width);
DEFINE_bool(only_compress_video,
            false,
            "Only compress video from existing images in --output_image_folder_name, default is false. If true, "
            "--output_filename and --output_image_folder_name must be provided. --output_fps, "
            "--output_image_name_prefix, and --output_image_name_field_width should match the image exporting settings"
            "if the default values are not used.");
DEFINE_uint64(limit_memory_usage_in_gb_to,
              0,
              "Limit memory usage to a specific GB value. Only valid for values >= 32. Default: 0 (no limit)");
DEFINE_int32(output_tile_size, 512, "Tile size for segmented rendering. Default: 512");
DEFINE_int32(output_tile_border, 64, "Tile border size for segmented rendering. Default: 64");
DEFINE_int32(maximum_output_width, 15360, "Maximum possible output video width. Default: 15360");
DEFINE_int32(maximum_output_height, 8640, "Maximum possible output video height. Default: 8640");

DEFINE_string(use_gpu_devices, "", "Comma-separated list of GPU device IDs to use (e.g., '0,1,2,3'). Linux only.");
DECLARE_uint32(use_gpu_device);

#if defined(__linux__)
DECLARE_bool(__use_EGL);
#endif

namespace nim {

int ZRunExport3DAnimation::run()
{
  LOG(INFO) << "Export 3D Animation Start";
  auto guard = folly::makeGuard([]() {
    LOG(INFO) << "Export 3D Animation End";
  });

  m_hasError = false;

  if (FLAGS_limit_memory_usage_in_gb_to >= 32) {
    ZCpuInfo::instance().setMemoryLimitInBytes(FLAGS_limit_memory_usage_in_gb_to * 1024 * 1024 * 1024);
  }

  auto outputFilename = QString::fromStdString(FLAGS_output_filename).trimmed();
  if (outputFilename.isEmpty()) {
    LOG(ERROR) << fmt::format("output file name ({}) is empty", FLAGS_output_filename);
    return 1;
  }
  if (!outputFilename.endsWith(".mp4", Qt::CaseInsensitive)) {
    outputFilename += ".mp4";
  }

  auto outputImageFolderName = QString::fromStdString(FLAGS_output_image_folder_name).trimmed();

  if (FLAGS_only_compress_video) {
    ZVideoEncoder videoEncoder;
    connect(&videoEncoder, &ZVideoEncoder::error, this, &ZRunExport3DAnimation::logError);
    QString namePrefix = QString::fromStdString(FLAGS_output_image_name_prefix);
    videoEncoder.encode(QDir(outputImageFolderName),
                        namePrefix,
                        FLAGS_output_image_name_field_width,
                        FLAGS_output_fps,
                        outputFilename);
    videoEncoder.waitForFinished(-1);
    LOG(INFO) << outputFilename << " saved";
    return m_hasError ? 1 : 0;
  }

  auto filename = QString::fromStdString(FLAGS_filename);
  if (!QFile::exists(filename)) {
    LOG(ERROR) << fmt::format("input file ({}) does not exist", FLAGS_filename);
    return 1;
  }

  if (FLAGS_output_start_frame == 0 && FLAGS_output_end_frame == -1) {
    if (FLAGS_output_start_time != 0.) {
      FLAGS_output_start_frame = FLAGS_output_start_time * FLAGS_output_fps;
    }
    if (FLAGS_output_end_time != -1.) {
      FLAGS_output_end_frame = FLAGS_output_end_time * FLAGS_output_fps;
    }
  }

#if defined(__linux__)
  if (std::vector<std::string_view> gpuDevices =
        absl::StrSplit(FLAGS_use_gpu_devices, absl::ByAnyChar(delimiter_literal), absl::SkipEmpty());
      !gpuDevices.empty()) {
    std::vector<uint32_t> gpuList;
    for (auto numStr : gpuDevices) {
      uint32_t v;
      if (!stringToValueNoThrow(numStr, v)) {
        LOG(ERROR) << fmt::format("invalid gpu device {}", numStr);
        return 1;
      }
      gpuList.push_back(v);
    }

    int totalEndFrame = FLAGS_output_end_frame;
    if (totalEndFrame <= 0) {
      ZDoc doc;
      QString errorMsg;
      if (size_t id = doc.animation3DDoc().loadFile(filename, errorMsg); id == 0) {
        LOG(ERROR) << "load animation file error: " << errorMsg;
        return 1;
      } else {
        totalEndFrame =
          std::max(1, static_cast<int>(std::ceil(doc.animation3DDoc().animation(id).duration() * FLAGS_output_fps)));
      }
    }

    if (gpuList.size() == 1 || totalEndFrame <= FLAGS_output_fps) {
      FLAGS_use_gpu_device = gpuList[0];
    } else {
      int nFramesForOneGPU = (totalEndFrame - FLAGS_output_start_frame) / static_cast<int>(gpuList.size());

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
        int startFrame = FLAGS_output_start_frame + int(idx) * nFramesForOneGPU;
        int endFrame = (idx + 1 == gpuList.size()) ? -1 : (startFrame + nFramesForOneGPU);
        gpuFutures.push_back(folly::via(cpuExecutor, [=]() {
          QStringList arguments;
          arguments << "--run_export_3d_animation"
                    << "--use_gpu_device" << QString::number(gpuList[idx]) << "--filename" << filename
                    << "--output_filename" << outputFilename << "--output_fps" << QString::number(FLAGS_output_fps)
                    << "--output_start_frame" << QString::number(startFrame) << "--output_end_frame"
                    << QString::number(endFrame) << "--output_width" << QString::number(FLAGS_output_width)
                    << "--output_height" << QString::number(FLAGS_output_height) << "--output_image_folder_name"
                    << outputImageFolderName << "--skip_video_compression"
                    << "--limit_memory_usage_in_gb_to"
                    << QString::number(FLAGS_limit_memory_usage_in_gb_to == 0
                                         ? 0
                                         : static_cast<int>(FLAGS_limit_memory_usage_in_gb_to / gpuList.size()))
                    << "--output_image_name_prefix" << QString::fromStdString(FLAGS_output_image_name_prefix)
                    << "--output_image_name_field_width" << QString::number(FLAGS_output_image_name_field_width)
                    << "--output_tile_size" << QString::number(FLAGS_output_tile_size) << "--output_tile_border"
                    << QString::number(FLAGS_output_tile_border);
          if (FLAGS_overwrite) {
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
        if (!FLAGS_skip_video_compression) {
          QStringList arguments;
          arguments << "--run_export_3d_animation"
                    << "--only_compress_video"
                    << "--output_filename" << outputFilename << "--output_image_folder_name" << outputImageFolderName
                    << "--output_fps" << QString::number(FLAGS_output_fps) << "--output_image_name_prefix"
                    << QString::fromStdString(FLAGS_output_image_name_prefix) << "--output_image_name_field_width"
                    << QString::number(FLAGS_output_image_name_field_width);
          if (FLAGS_overwrite) {
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

      return 0;
    }
  }

  FLAGS___use_EGL = true;
#else
  if (auto gpuDevices = QString::fromStdString(FLAGS_use_gpu_devices).trimmed(); !gpuDevices.isEmpty()) {
    LOG(ERROR) << "Flag --use_gpu_devices is Linux only";
  }
#endif

  ZDoc doc;
  Z3DRenderingEngine engine(doc);
  engine.init();

  QString errorMsg;
  size_t id;
  if (id = doc.animation3DDoc().loadFile(filename, errorMsg); id == 0) {
    LOG(ERROR) << "load animation file error: " << errorMsg;
    return 1;
  }

  doc.animation3DDoc().bindView(&engine);

  connect(&engine, &Z3DRenderingEngine::renderingError, this, &ZRunExport3DAnimation::logError);

  engine.exportFixedSize3DAnimation(&doc.animation3DDoc().animation(id),
                                    outputFilename,
                                    FLAGS_output_fps,
                                    FLAGS_output_start_frame,
                                    FLAGS_output_end_frame,
                                    FLAGS_output_width,
                                    FLAGS_output_height,
                                    FLAGS_overwrite,
                                    Z3DScreenShotType::MonoView,
                                    nullptr,
                                    outputImageFolderName.isEmpty() ? nullptr : &outputImageFolderName,
                                    FLAGS_skip_video_compression,
                                    FLAGS_output_tile_size,
                                    FLAGS_output_tile_border);

  return m_hasError ? 1 : 0;
}

void ZRunExport3DAnimation::logError(const QString& err)
{
  LOG(ERROR) << err;
  m_hasError = true;
}

} // namespace nim
