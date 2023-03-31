#include "zrunexport3danimation.h"

#include "zlog.h"
#include "zdoc.h"
#include "z3drenderingengine.h"
#include "z3danimationdoc.h"
#include <folly/ScopeGuard.h>

DEFINE_bool(run_export_3d_animation, false, "run exporting 3d animation in command line mode");
DEFINE_string(filename, "", "input filename");
DEFINE_string(output_filename, "", "output filename");
DEFINE_double(output_fps, 30, "frame per second of the output video");
DEFINE_double(output_start_time, 0., "start time of the output video");
DEFINE_double(output_end_time, -1., "end time of the output video");
DEFINE_int32(output_width, 3840, "width of the output video");
DEFINE_int32(output_height, 2160, "height of the output video");
DEFINE_bool(overwrite, false, "whether to overwrite output file if it already exists");
DEFINE_string(output_image_folder_name, "", "output folder for images, use temporary folder if not provided");
DEFINE_bool(skip_video_compression, false, "whether to skip video compression, if true, --output_image_folder_name must be provided");
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

  auto outputFilename = QString::fromStdString(FLAGS_output_filename).trimmed();
  if (outputFilename.isEmpty()) {
    LOG(ERROR) << fmt::format("output file name ({}) is empty", FLAGS_output_filename);
    return 1;
  }
  if (!outputFilename.endsWith(".mp4", Qt::CaseInsensitive)) {
    outputFilename += ".mp4";
  }

  auto outputImageFolderName = QString::fromStdString(FLAGS_output_image_folder_name).trimmed();

  doc.animation3DDoc().bindView(&engine);

  connect(&engine, &Z3DRenderingEngine::renderingError, &ZRunExport3DAnimation::logError);

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

  return 0;
}

void ZRunExport3DAnimation::logError(const QString& err)
{
  LOG(ERROR) << err;
}

} // namespace nim
