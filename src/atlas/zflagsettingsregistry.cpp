#include "zflagsettingsregistry.h"

#include "zsysteminfo.h"
#include "zlog.h"

#include <gflags/gflags.h>

namespace nim {
namespace {

ZFlagSettingSpec makeSpec(QString name,
                          QString category,
                          QString label,
                          ZFlagSettingEditorKind editor = ZFlagSettingEditorKind::Auto,
                          QStringList choices = {},
                          bool advanced = false)
{
  ZFlagSettingSpec spec;
  spec.name = std::move(name);
  spec.category = std::move(category);
  spec.label = std::move(label);
  spec.editor = editor;
  spec.choices = std::move(choices);
  spec.advanced = advanced;
  return spec;
}

bool flagExistsInCurrentBuild(const QString& name)
{
  gflags::CommandLineFlagInfo info;
  const QByteArray flagName = name.toUtf8();
  return gflags::GetCommandLineFlagInfo(flagName.constData(), &info);
}

} // namespace

const std::vector<ZFlagSettingSpec>& atlasFlagSettingSpecs()
{
  static const std::vector<ZFlagSettingSpec> specs = []() {
    std::vector<ZFlagSettingSpec> allSpecs;

    allSpecs.push_back(makeSpec("atlas_image_cache_memory_proportion", "Memory & Cache", "Image cache RAM proportion"));
    allSpecs.push_back(
      makeSpec("atlas_image_region_cache_memory_proportion", "Memory & Cache", "Image-region cache RAM proportion"));
    allSpecs.push_back(makeSpec("atlas_ng_precomputed_chunk_cache_memory_proportion",
                                "Memory & Cache",
                                "Neuroglancer chunk cache RAM proportion"));
    allSpecs.push_back(makeSpec("atlas_ng_precomputed_minishard_index_cache_memory_proportion",
                                "Memory & Cache",
                                "Neuroglancer minishard cache RAM proportion"));
    allSpecs.push_back(makeSpec("atlas_disk_cache_http_max_bytes", "Memory & Cache", "HTTP disk cache max bytes"));
    allSpecs.push_back(
      makeSpec("atlas_disk_cache_imgregion_max_bytes", "Memory & Cache", "Image-region disk cache max bytes"));
    allSpecs.push_back(
      makeSpec("atlas_disk_cache_imgpreview_max_bytes", "Memory & Cache", "3D preview disk cache max bytes"));
    allSpecs.push_back(makeSpec("atlas_disk_cache_imgpreview_async_max_pending_bytes",
                                "Memory & Cache",
                                "3D preview cache async queue max bytes",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_disk_cache_sqlite_reader_cache_bytes",
                                "Memory & Cache",
                                "Disk-cache SQLite reader cache bytes",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_disk_cache_sqlite_writer_cache_bytes",
                                "Memory & Cache",
                                "Disk-cache SQLite writer cache bytes",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_disk_cache_sqlite_mmap_bytes",
                                "Memory & Cache",
                                "Disk-cache SQLite mmap bytes",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_disk_cache_sqlite_journal_size_limit_bytes",
                                "Memory & Cache",
                                "Disk-cache SQLite journal size limit",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_disk_cache_sqlite_touch_min_interval_seconds",
                                "Memory & Cache",
                                "Disk-cache touch-on-read interval (s)",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_disk_cache_sqlite_page_size",
                                "Memory & Cache",
                                "Disk-cache SQLite page size",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_3d_preview_max_dimension", "Memory & Cache", "3D preview max dimension"));
    allSpecs.push_back(makeSpec("atlas_disk_cache_dir",
                                "Memory & Cache",
                                "Disk cache root directory",
                                ZFlagSettingEditorKind::DirectoryPath));

    allSpecs.push_back(makeSpec("atlas_http_backend",
                                "Network & Remote Data",
                                "HTTP backend",
                                ZFlagSettingEditorKind::Choice,
                                {"proxygen", "curl"}));
    allSpecs.push_back(makeSpec("atlas_http_ca_bundle",
                                "Network & Remote Data",
                                "HTTPS CA bundle path",
                                ZFlagSettingEditorKind::FilePath));
#ifdef _WIN32
    allSpecs.push_back(makeSpec("atlas_http_windows_trust_source",
                                "Network & Remote Data",
                                "Windows HTTPS trust source",
                                ZFlagSettingEditorKind::Choice,
                                {"auto", "windows_store", "bundled_pem"}));
#endif
    allSpecs.push_back(makeSpec("atlas_http_proxy_strategy",
                                "Network & Remote Data",
                                "Proxy strategy",
                                ZFlagSettingEditorKind::Choice,
                                {"auto", "no_proxy", "proxy_if_available"}));
    allSpecs.push_back(makeSpec("atlas_http_max_retries", "Network & Remote Data", "HTTP max retries"));
    allSpecs.push_back(
      makeSpec("atlas_http_retry_backoff_initial_ms", "Network & Remote Data", "HTTP retry initial backoff (ms)"));
    allSpecs.push_back(
      makeSpec("atlas_http_retry_backoff_max_ms", "Network & Remote Data", "HTTP retry max backoff (ms)"));

    allSpecs.push_back(makeSpec("atlas_readRegionToImg_use_multithreaded_resize",
                                "I/O & Processing",
                                "Use multithreaded resize for readRegionToImg",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_bioformats_bridge_worker_count",
                                "I/O & Processing",
                                "Bio-Formats bridge worker count",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_bioformats_bridge_use_grpc",
                                "I/O & Processing",
                                "Use Bio-Formats gRPC bridge",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_number_of_blocks_to_use_PBO_threashold",
                                "I/O & Processing",
                                "PBO upload block threshold",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("zimg_global_fft_number_of_threads", "I/O & Processing", "FFT thread count"));
    allSpecs.push_back(
      makeSpec("zimg_use_mkl_for_fft_if_available", "I/O & Processing", "Use MKL for FFT when available"));
    allSpecs.push_back(makeSpec("zimg_use_mmap_file_for_hdf5",
                                "I/O & Processing",
                                "Use mmap file for HDF5 / NIM",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));

    allSpecs.push_back(makeSpec("atlas_default_render_backend",
                                "Rendering",
                                "Default 3D render backend",
                                ZFlagSettingEditorKind::Choice,
                                {"opengl", "vulkan"}));
    allSpecs.push_back(
      makeSpec("atlas_volume_rendering_maximum_round", "Rendering", "Maximum volume rendering rounds"));
    allSpecs.push_back(makeSpec("atlas_image_block_size",
                                "Rendering",
                                "3D image block size",
                                ZFlagSettingEditorKind::Choice,
                                {"64", "128", "256", "512"}));
    allSpecs.push_back(makeSpec("atlas_vk_copy_yflip_in_shader",
                                "Rendering",
                                "Use Vulkan shader Y-flip for final copy",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_mesh_preferred_triangle_budget_per_segment",
                                "Rendering",
                                "Mesh preferred triangles per segment",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_sphere_preferred_instance_budget_per_segment",
                                "Rendering",
                                "Sphere preferred instances per segment",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_cone_preferred_instance_budget_per_segment",
                                "Rendering",
                                "Cone preferred instances per segment",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_ellipsoid_preferred_instance_budget_per_segment",
                                "Rendering",
                                "Ellipsoid preferred instances per segment",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_line_preferred_segment_budget_per_segment",
                                "Rendering",
                                "Line preferred logical segments per segment",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));

    allSpecs.push_back(makeSpec("atlas_log_folly_global_executor_status_interval_in_seconds",
                                "Logging & Debugging",
                                "Folly executor status log interval (s)",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_debug_opengl",
                                "Logging & Debugging",
                                "Enable OpenGL debug checks",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_log_3d_paging_frame_stats",
                                "Logging & Debugging",
                                "Log 3D paging frame stats",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_log_3d_paging_round_stats",
                                "Logging & Debugging",
                                "Log 3D paging round stats",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_log_glbinding_context_switch",
                                "Logging & Debugging",
                                "Log OpenGL context switches",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("atlas_debug_vulkan",
                                "Logging & Debugging",
                                "Enable Vulkan debug features",
                                ZFlagSettingEditorKind::Auto,
                                {},
                                true));
    allSpecs.push_back(makeSpec("v", "Logging & Debugging", "Global log verbosity"));

    std::vector<ZFlagSettingSpec> filteredSpecs;
    filteredSpecs.reserve(allSpecs.size());
    for (const auto& spec : allSpecs) {
      if (!flagExistsInCurrentBuild(spec.name)) {
        LOG(WARNING) << "Skipping settings registry flag missing from this Atlas build: " << spec.name.toStdString();
        continue;
      }
      filteredSpecs.push_back(spec);
    }

    return filteredSpecs;
  }();

  return specs;
}

const ZFlagSettingSpec* atlasFindFlagSettingSpec(const QString& name)
{
  const auto& specs = atlasFlagSettingSpecs();
  for (const auto& spec : specs) {
    if (spec.name == name) {
      return &spec;
    }
  }
  return nullptr;
}

QSet<QString> atlasManagedFlagNames()
{
  QSet<QString> names;
  const auto& specs = atlasFlagSettingSpecs();
  for (const auto& spec : specs) {
    names.insert(spec.name);
  }
  return names;
}

QString atlasUserSettingsFlagfileName()
{
  return QStringLiteral("user_settings_flagfile.txt");
}

QString atlasUserSettingsFlagfilePath()
{
  return ZSystemInfo::configDir().absoluteFilePath(atlasUserSettingsFlagfileName());
}

std::vector<ZManagedFlagfileEntry> atlasDefaultFlagfileEntries()
{
  std::vector<ZManagedFlagfileEntry> entries;
  const auto& specs = atlasFlagSettingSpecs();
  entries.reserve(specs.size());

  for (const auto& spec : specs) {
    const QByteArray flagName = spec.name.toUtf8();
    const auto info = gflags::GetCommandLineFlagInfoOrDie(flagName.constData());

    ZManagedFlagfileEntry entry;
    entry.category = spec.category;
    entry.label = spec.label;
    entry.name = spec.name;
    entry.description = QString::fromStdString(info.description);
    entry.value = QString::fromStdString(info.default_value);
    entries.push_back(std::move(entry));
  }

  return entries;
}

} // namespace nim
