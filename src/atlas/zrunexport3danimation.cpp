#include "zrunexport3danimation.h"

#include "zlog.h"
#include "zdoc.h"
#include "z3drenderingengine.h"
#include "z3danimationdoc.h"
#include "zvideoencoder.h"
#include "zcpuinfo.h"
#include <folly/ScopeGuard.h>

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
DECLARE_uint32(use_gpu_device);
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
    return 0;
  }

#if defined(__linux__)
  FLAGS___use_EGL = true;
#endif

  ZDoc doc;
  Z3DRenderingEngine engine(doc);
  engine.init();

  auto filename = QString::fromStdString(FLAGS_filename);
  if (!QFile::exists(filename)) {
    LOG(ERROR) << fmt::format("input file ({}) does not exist", FLAGS_filename);
    return 1;
  }

  QString errorMsg;
  size_t id;
  if (id = doc.animation3DDoc().loadFile(filename, errorMsg); id == 0) {
    LOG(ERROR) << "load animation file error: " << errorMsg;
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
