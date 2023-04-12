#include "zrunexport3danimation.h"

#include "zlog.h"
#include "zdoc.h"
#include "z3drenderingengine.h"
#include "z3danimationdoc.h"
#include "zvideoencoder.h"
#include "zcpuinfo.h"
#include <folly/ScopeGuard.h>

DEFINE_bool(run_export_3d_animation, false, "run exporting 3d animation in command line mode");
DEFINE_string(filename, "", "input filename");
DEFINE_string(output_filename, "", "output video filename");
DEFINE_int32(output_fps, 30, "frame per second of the output video, default is 30");
DEFINE_double(output_start_time, 0., "start time of the output video in seconds, floating point value, default is 0.0");
DEFINE_double(output_end_time, -1., "end time of the output video in seconds, floating point value, default is -1.0 represents the end of the animation");
DEFINE_int32(output_width, 3840, "width of the output video, default is 3840");
DEFINE_int32(output_height, 2160, "height of the output video, default is 2160");
DEFINE_bool(overwrite, false, "whether to overwrite output file if it already exists, default is false");
DEFINE_string(output_image_folder_name, "", "output folder for images, will use temporary folder if not provided");
DEFINE_bool(
  skip_video_compression,
  false,
  "whether to skip video compression, default is false, if true, --output_image_folder_name must be provided");
DECLARE_uint32(use_gpu_device);
DECLARE_string(output_image_name_prefix);
DECLARE_int32(output_image_name_field_width);
DEFINE_bool(only_compress_video,
            false,
            "compress video if images are already in --output_image_folder_name, default is false, if true, "
            "--output_filename and --output_image_folder_name must be provided. --output_fps, "
            "--output_image_name_prefix, and --output_image_name_field_width should match the image exporting settings"
            "if the default values are not used.");
DEFINE_uint64(
  limit_memory_usage_in_gb_to,
  0,
  "limit memory usage to less than XX GB, has no effect if the available memory is less than the input value, "
  "only value larger than or equal to 32 will be accepted as valid limit, default is 0 means no limit");

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

  doc.animation3DDoc().bindView(&engine);

  connect(&engine, &Z3DRenderingEngine::renderingError, this, &ZRunExport3DAnimation::logError);

  engine.exportFixedSize3DAnimation(&doc.animation3DDoc().animation(id),
                                    outputFilename,
                                    FLAGS_output_fps,
                                    FLAGS_output_start_time,
                                    FLAGS_output_end_time,
                                    FLAGS_output_width,
                                    FLAGS_output_height,
                                    FLAGS_overwrite,
                                    Z3DScreenShotType::MonoView,
                                    nullptr,
                                    outputImageFolderName.isEmpty() ? nullptr : &outputImageFolderName,
                                    FLAGS_skip_video_compression);

  return m_hasError ? 1 : 0;
}

void ZRunExport3DAnimation::logError(const QString& err)
{
  LOG(ERROR) << err;
  m_hasError = true;
}

} // namespace nim
