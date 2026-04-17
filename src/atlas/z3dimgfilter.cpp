#include "z3dimgfilter.h"

#include "z3dgpuinfo.h"
#include "zbenchtimer.h"
#include "zeventlistenerparameter.h"
#include "zlog.h"
#include "z3drendertarget.h"
#include "z3drenderglobalstate.h"
#include "zmesh.h"
#include "zcancellation.h"
#include "zneuroglancerprecomputed.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanlinearscript.h"
#include "zvulkantexture.h"
#include <folly/OperationCancelled.h>
#include <folly/ScopeGuard.h>
#include <glm/ext/matrix_projection.hpp>
#include <QMenu>
#include <QPushButton>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

DECLARE_bool(atlas_enable_benchmark_raw_mip_export);
DECLARE_bool(atlas_enable_benchmark_screen_space_sufficiency_audit);
DECLARE_bool(atlas_volume_rendering_analytic_ray_setup);

namespace nim {

// const size_t Z3DImgFilter::m_maxNumOfFullResolutionVolumeSlice = 6;

namespace {

constexpr double kPresetDomainMin = 0.0;
constexpr double kPresetDomainMax = 1.0;
constexpr uint32_t kSliceColormapLutWidth = 256u;
constexpr double kEmPresetOpaqueThreshold = 4.5 / 255.0;

[[nodiscard]] double emPresetOpaqueThreshold(uint32_t lutWidth)
{
  (void)lutWidth;
  return kEmPresetOpaqueThreshold;
}

[[nodiscard]] glm::col4 emPresetOpaqueColor(const col4& channelColor)
{
  return glm::col4(channelColor.r, channelColor.g, channelColor.b, 255);
}

[[nodiscard]] std::vector<ZColorMapKey> emPresetKeys(uint32_t lutWidth, const glm::col4& channelColor)
{
  const double threshold = emPresetOpaqueThreshold(lutWidth);
  const glm::col4 transparentBlack(0, 0, 0, 0);
  const glm::col4 opaqueBlack(0, 0, 0, 255);
  return {ZColorMapKey(kPresetDomainMin, transparentBlack),
          ZColorMapKey(threshold, transparentBlack, opaqueBlack),
          ZColorMapKey(kPresetDomainMax, glm::col4(channelColor.r, channelColor.g, channelColor.b, 255))};
}

[[nodiscard]] glm::mat4 localToTexMatrix(const glm::vec3& coordLuf, const glm::vec3& coordRdb)
{
  const glm::vec3 extent = coordRdb - coordLuf;
  CHECK(std::abs(extent.x) > 1e-6f && std::abs(extent.y) > 1e-6f && std::abs(extent.z) > 1e-6f)
    << "Invalid image physical extent for analytic ray setup: (" << extent.x << ", " << extent.y << ", " << extent.z
    << ")";
  const glm::vec3 invExtent = 1.0f / extent;

  glm::mat4 transform(1.0f);
  transform[0][0] = invExtent.x;
  transform[1][1] = invExtent.y;
  transform[2][2] = invExtent.z;
  transform[3] = glm::vec4(-coordLuf * invExtent, 1.0f);
  return transform;
}

[[nodiscard]] glm::mat4 fragCoordToNdcMatrix(const glm::uvec2& outputSize)
{
  CHECK(outputSize.x > 0u && outputSize.y > 0u)
    << "Analytic ray setup requires a non-zero output size for fragCoord reconstruction";
  const glm::vec3 scale(2.0f / static_cast<float>(outputSize.x), 2.0f / static_cast<float>(outputSize.y), 1.0f);
  return glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, -1.0f, 0.0f)) * glm::scale(glm::mat4(1.0f), scale);
}

} // namespace

Z3DImgFilter::Z3DImgFilter(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DBoundedFilter(globalParas, parent)
  , m_imgRaycasterRenderer(m_rendererBase)
  , m_imgSliceRenderer(m_rendererBase)
  , m_textureCopyRenderer(m_rendererBase)
  , m_stayOnTop("Stay On Top", false)
  , m_fullResolutionRendering("Full Resolution Rendering", false)
  , m_raycasterCompositingMode("Compositing")
  , m_raycasterSamplingRate("Sampling Rate", 2.f, 0.01f, 20.f)
  , m_raycasterIsoValue("ISO Value", 0.5f, 0.0f, 1.0f)
  , m_raycasterLocalMIPThreshold("Local MIP Threshold", 0.8f, 0.01f, 1.f)
  , m_numParas(0)
  //, m_interactionDownsample("Interaction Downsample", 1, 1, 16)
  //, m_smoothInteraction("Smooth Interaction", true)
  //, m_FRVolumeSlices(m_maxNumOfFullResolutionVolumeSlice)
  //, m_FRVolumeSlicesValidState(m_maxNumOfFullResolutionVolumeSlice, false)
  //, m_useFRVolumeSlice("Use Full Resolution Volume Slice", true)
  , m_showXSlice("Show X Slice", false)
  , m_xSlicePosition("X Slice Position", 0, 0, 1)
  , m_showYSlice("Show Y Slice", false)
  , m_ySlicePosition("Y Slice Position", 0, 0, 1)
  , m_showZSlice("Show Z Slice", false)
  , m_zSlicePosition("Z Slice Position", 0, 0, 1)
  , m_showObliqueSlice("Show Oblique Slice", false)
  , m_obliqueSliceNormal("Oblique Slice Normal", glm::vec3(1, 1, 0), glm::vec3(-1, -1, -1), glm::vec3(1, 1, 1))
  , m_obliqueSliceDistanceToOrigin("Oblique Slice Distance to Origin", 0, 0, 0)
  , m_showObliqueSlice2("Show Oblique Slice 2", false)
  , m_obliqueSlice2Normal("Oblique Slice 2 Normal", glm::vec3(1, 1, 0), glm::vec3(-1, -1, -1), glm::vec3(1, 1, 1))
  , m_obliqueSlice2DistanceToOrigin("Oblique Slice 2 Distance to Origin", 0, 0, 0)
  , m_showXSlice2("Show X Slice 2", false)
  , m_xSlice2Position("X Slice 2 Position", 0, 0, 1)
  , m_showYSlice2("Show Y Slice 2", false)
  , m_ySlice2Position("Y Slice 2 Position", 0, 0, 1)
  , m_showZSlice2("Show Z Slice 2", false)
  , m_zSlice2Position("Z Slice 2 Position", 0, 0, 1)
  , m_leftMouseButtonPressEvent("Left Mouse Button Pressed", false)
  , m_contextMenuEvent("Context Menu", false)
{
  m_baseBoundBoxRenderer.setFollowSupersampling(false);
  m_textureCopyRenderer.setDiscardTransparent(true);

  updateRaycasterSamplingRate();
  updateRaycasterIsoValue();
  updateRaycasterLocalMIPThreshold();

  connect(&m_raycasterSamplingRate, &ZFloatParameter::valueChanged, this, &Z3DImgFilter::updateRaycasterSamplingRate);
  connect(&m_raycasterIsoValue, &ZFloatParameter::valueChanged, this, &Z3DImgFilter::updateRaycasterIsoValue);
  connect(&m_raycasterLocalMIPThreshold,
          &ZFloatParameter::valueChanged,
          this,
          &Z3DImgFilter::updateRaycasterLocalMIPThreshold);

  m_raycasterCompositingMode.clearOptions();
  m_raycasterCompositingMode.addOptionsWithData(
    std::make_pair(QStringLiteral("Direct Volume Rendering"),
                   static_cast<int>(ImgCompositingMode::DirectVolumeRendering)),
    std::make_pair(QStringLiteral("Maximum Intensity Projection"),
                   static_cast<int>(ImgCompositingMode::MaximumIntensityProjection)),
    std::make_pair(QStringLiteral("MIP Opaque"), static_cast<int>(ImgCompositingMode::MIPOpaque)),
    std::make_pair(QStringLiteral("Local MIP"), static_cast<int>(ImgCompositingMode::LocalMIP)),
    std::make_pair(QStringLiteral("Local MIP Opaque"), static_cast<int>(ImgCompositingMode::LocalMIPOpaque)),
    std::make_pair(QStringLiteral("ISO Surface"), static_cast<int>(ImgCompositingMode::IsoSurface)),
    std::make_pair(QStringLiteral("X Ray"), static_cast<int>(ImgCompositingMode::XRay)));
  m_raycasterCompositingMode.select(QStringLiteral("MIP Opaque"));
  m_raycasterCompositingMode.setDescription(QStringLiteral(
    "Volume compositing mode. ISO Surface uses 'ISO Value'; MIP/Local MIP emphasize bright voxels;"
    " Direct Volume Rendering integrates color/opacity; X-Ray is a fast approximant."));

  updateRaycasterCompositingMode();
  connect(&m_raycasterCompositingMode,
          &ZStringIntOptionParameter::valueChanged,
          this,
          &Z3DImgFilter::updateRaycasterCompositingMode);

  addParameter(m_stayOnTop);
  m_stayOnTop.setDescription(QStringLiteral(
    "Render this volume layer on top of other geometry."));
  addParameter(m_fullResolutionRendering);
  m_fullResolutionRendering.setDescription(QStringLiteral(
    "Render at full resolution instead of adaptive downsampling (higher quality, slower)."));
  connect(this, &Z3DBoundedFilter::rendererCoordTransformChanged, this, &Z3DImgFilter::changeCoordTransform);
  //  connect(&m_globalParameters.interactionHandler,
  //          &Z3DTrackballInteractionHandler::enterInteractionMode,
  //          this,
  //          &Z3DImgFilter::enterFastMode);
  //  connect(&m_globalParameters.interactionHandler,
  //          &Z3DTrackballInteractionHandler::exitInteractionMode,
  //          this,
  //          &Z3DImgFilter::exitFastMode);

  // addParameter(m_interactionDownsample);
  // addParameter(m_smoothInteraction);

  // renderer-owned targets will be sized on demand

  // layer, block-id, and progressive render targets are now owned by renderers

  markTargetsInvalid();
  connect(&m_visible, &ZBoolParameter::boolChanged, this, &Z3DImgFilter::onVisibilityChanged);

  m_obliqueSliceNormal.setNameForEachValue({"x", "y", "z"});
  m_obliqueSlice2Normal.setNameForEachValue({"x", "y", "z"});

  // addParameter(m_useFRVolumeSlice);
  addParameter(m_showXSlice);
  m_showXSlice.setDescription(QStringLiteral("Toggle orthogonal X slice visibility."));
  addParameter(m_xSlicePosition);
  m_xSlicePosition.setDescription(QStringLiteral("Normalized X slice position (0..1)."));
  addParameter(m_showYSlice);
  m_showYSlice.setDescription(QStringLiteral("Toggle orthogonal Y slice visibility."));
  addParameter(m_ySlicePosition);
  m_ySlicePosition.setDescription(QStringLiteral("Normalized Y slice position (0..1)."));
  addParameter(m_showZSlice);
  m_showZSlice.setDescription(QStringLiteral("Toggle orthogonal Z slice visibility."));
  addParameter(m_zSlicePosition);
  m_zSlicePosition.setDescription(QStringLiteral("Normalized Z slice position (0..1)."));
  addParameter(m_showObliqueSlice);
  m_showObliqueSlice.setDescription(QStringLiteral("Toggle an arbitrary oblique slice plane."));
  addParameter(m_obliqueSliceNormal);
  m_obliqueSliceNormal.setDescription(QStringLiteral("Oblique slice plane normal (unit vector)."));
  addParameter(m_obliqueSliceDistanceToOrigin);
  m_obliqueSliceDistanceToOrigin.setDescription(QStringLiteral("Signed distance from origin for oblique slice plane."));
  addParameter(m_showObliqueSlice2);
  m_showObliqueSlice2.setDescription(QStringLiteral("Toggle a second oblique slice plane."));
  addParameter(m_obliqueSlice2Normal);
  m_obliqueSlice2Normal.setDescription(QStringLiteral("Second oblique slice plane normal (unit vector)."));
  addParameter(m_obliqueSlice2DistanceToOrigin);
  m_obliqueSlice2DistanceToOrigin.setDescription(QStringLiteral("Signed distance from origin for second oblique slice plane."));
  addParameter(m_showXSlice2);
  m_showXSlice2.setDescription(QStringLiteral("Toggle second X slice visibility."));
  addParameter(m_xSlice2Position);
  m_xSlice2Position.setDescription(QStringLiteral("Normalized second X slice position (0..1)."));
  addParameter(m_showYSlice2);
  m_showYSlice2.setDescription(QStringLiteral("Toggle second Y slice visibility."));
  addParameter(m_ySlice2Position);
  m_ySlice2Position.setDescription(QStringLiteral("Normalized second Y slice position (0..1)."));
  addParameter(m_showZSlice2);
  m_showZSlice2.setDescription(QStringLiteral("Toggle second Z slice visibility."));
  addParameter(m_zSlice2Position);
  m_zSlice2Position.setDescription(QStringLiteral("Normalized second Z slice position (0..1)."));

  connect(&m_showXSlice, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showYSlice, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showZSlice, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showObliqueSlice, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showObliqueSlice2, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showXSlice2, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showYSlice2, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showZSlice2, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);

  // connect(&m_xSlicePosition, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeXSlice);
  // connect(&m_ySlicePosition, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeYSlice);
  // connect(&m_zSlicePosition, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeZSlice);
  // connect(&m_xSlice2Position, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeXSlice2);
  // connect(&m_ySlice2Position, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeXSlice2);
  // connect(&m_zSlice2Position, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeXSlice2);

  m_leftMouseButtonPressEvent.listenTo("trace", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
  m_leftMouseButtonPressEvent.listenTo("trace", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonRelease);
  connect(&m_leftMouseButtonPressEvent,
          &ZEventListenerParameter::mouseEventTriggered,
          this,
          &Z3DImgFilter::leftMouseButtonPressed);
  addEventListener(m_leftMouseButtonPressEvent);

  m_contextMenuEvent.listenToContextMenuEvent();
  connect(&m_contextMenuEvent,
          &ZEventListenerParameter::contextMenuEventTriggered,
          this,
          &Z3DImgFilter::contextMenuEvent);
  addEventListener(m_contextMenuEvent);

  m_boundBoxLineWidth.set(1);
  m_boundBoxMode.select("Bound Box");

  addParameter(m_raycasterCompositingMode);
  addParameter(m_raycasterIsoValue);
  addParameter(m_raycasterLocalMIPThreshold);
  addParameter(m_raycasterSamplingRate);
  m_raycasterSamplingRate.setDescription(QStringLiteral(
    "Raymarching step size multiplier. Smaller values improve quality and cost more."));

  m_imgRaycasterRenderer.setFastRendering(!m_fullResolutionRendering.get());
  m_imgSliceRenderer.setFastRendering(!m_fullResolutionRendering.get());
  connect(&m_fullResolutionRendering,
          &ZBoolParameter::valueChanged,
          this,
          &Z3DImgFilter::fullResolutionRenderingToggled);

  adjustWidget();

  m_numParas = m_parameters.size();

  for (auto* para : m_globalParameters.parameters()) {
    connect(para, &ZParameter::valueChanged, this, &Z3DBoundedFilter::invalidateResult);
  }
}

Z3DImgFilter::~Z3DImgFilter()
{
  // Z3DImg owns Vulkan paging caches (managed textures) via destruction callbacks.
  // Those caches are pinned per submission by the Vulkan backend; ensure all in-flight
  // work and post-fence unpin callbacks have drained before m_3dImg is destroyed.
  m_rendererBase.flushVulkanWorkForTeardown("Z3DImgFilter::~Z3DImgFilter");
}

void Z3DImgFilter::setData(const ZImgPack& imgPack)
{
  if (m_widgetsGroup) {
    for (const auto& para : m_channelVisibleParas) {
      m_widgetsGroup->removeChild(*para);
    }
    for (const auto& para : m_doubleChannelRangeParas) {
      m_widgetsGroup->removeChild(*para);
    }
    for (const auto& para : m_transferFuncParas) {
      m_widgetsGroup->removeChild(*para);
    }
    //    for (const auto& para : m_imgRaycasterRenderer.texFilterModeParas()) {
    //      m_widgetsGroup->removeChild(*para);
    //    }
    for (const auto& cm : m_sliceColormaps) {
      m_widgetsGroup->removeChild(*cm);
    }
  }
  while (m_numParas < m_parameters.size()) {
    removeParameter(*m_parameters[m_numParas]);
  }

  try {
    m_channelVisibleParas.clear();
    m_transferFuncParas.clear();

    // Z3DImg holds a reference to the provided ZImgPack, so any adapter pack
    // must live as long as m_3dImg.
    // For Vulkan, ensure any in-flight submissions have released their residency pins
    // before destroying the previous Z3DImg (owner-release triggers residency cleanup).
    m_rendererBase.flushVulkanWorkForTeardown("Z3DImgFilter::setData");
    m_3dImg.reset();
    m_imgPackOverride.reset();
    const ZImgPack* packFor3D = &imgPack;
    if (imgPack.isNeuroglancerPrecomputed()) {
      auto vol = imgPack.neuroglancerVolumeShared();
      if (vol->isSegmentation()) {
        m_imgPackOverride = imgPack.makeNeuroglancerSegmentationRgbFor3D();
        CHECK(m_imgPackOverride);
        packFor3D = m_imgPackOverride.get();
        LOG(INFO) << "Neuroglancer segmentation: using RGB adapter pack for 3D visualization.";
      }
    }

    // For datasets that provide physical voxel spacing, initialize the coordinate transform
    // scale from voxel-size *ratios* (unitless). This improves both visual aspect and paging
    // LOD behavior for anisotropic volumes (e.g. Z resolution coarser than XY).
    //
    // Only apply this auto-scale when the user has not customized the scale (or the current
    // scale is the one we previously auto-applied).
    const glm::vec3 currentScale = m_rendererParameters.coordTransform.scale();
    const bool isDefaultScale = glm::all(glm::epsilonEqual(currentScale, glm::vec3(1.f), 1e-6f));
    const bool isPreviousAutoScale =
      m_hasAutoVoxelAspectScale && glm::all(glm::epsilonEqual(currentScale, m_autoVoxelAspectScale, 1e-6f));
    if (isDefaultScale || isPreviousAutoScale) {
      const ZImgInfo& info = packFor3D->imgInfo();
      if (info.voxelSizeUnit != VoxelSizeUnit::none && std::isfinite(info.voxelSizeX) && std::isfinite(info.voxelSizeY) &&
          std::isfinite(info.voxelSizeZ) && info.voxelSizeX > 0.0 && info.voxelSizeY > 0.0 && info.voxelSizeZ > 0.0) {
        const double xy = std::max(info.voxelSizeX, info.voxelSizeY);
        const double zOverXY = info.voxelSizeZ / xy;
        if (std::isfinite(zOverXY) && zOverXY > 0.0) {
          const glm::vec3 suggestedScale(1.f, 1.f, static_cast<float>(zOverXY));
          m_hasAutoVoxelAspectScale = true;
          m_autoVoxelAspectScale = suggestedScale;
          m_rendererParameters.coordTransform.setScale(suggestedScale);
          LOG(INFO) << fmt::format(
            "3D: using voxel-size aspect ratio for coordTransform scale: "
            "voxelSize=({:.6g},{:.6g},{:.6g}) -> scale=(1,1,{:.6g})",
            info.voxelSizeX,
            info.voxelSizeY,
            info.voxelSizeZ,
            zOverXY);
        }
      }
    }

    std::vector<glm::dvec2> drs;
    if (packFor3D->imgInfo().isType<uint8_t>()) {
      drs = std::vector<glm::dvec2>(packFor3D->imgInfo().numChannels, glm::dvec2(0, 255));
    } else if (packFor3D->hasMinMax() && packFor3D->maxIntensity() > packFor3D->minIntensity()) {
      drs = std::vector<glm::dvec2>(
        packFor3D->imgInfo().numChannels,
        glm::dvec2(packFor3D->minIntensity() + (packFor3D->maxIntensity() - packFor3D->minIntensity()) * 0.02,
                   packFor3D->maxIntensity()));
    } else {
      drs = std::vector<glm::dvec2>(
        packFor3D->imgInfo().numChannels,
        glm::dvec2(packFor3D->rangeMin() + (packFor3D->rangeMax() - packFor3D->rangeMin()) * 0.02,
                   packFor3D->rangeMax()));
    }

    m_3dImg = std::make_unique<Z3DImg>(*packFor3D, m_rendererParameters.coordTransform.scale(), drs);
    updateBlockIDTarget();

    // Layer target channel depth managed inside renderers now
    m_fullResolutionRendering.set(!m_3dImg->isVolumeDownsampled());
    m_fullResolutionRendering.setEnabled(m_3dImg->isVolumeDownsampled());
    // m_smoothInteraction.setVisible(m_3dImg->isVolumeDownsampled());

    m_sliceColormaps.clear();
    m_doubleChannelRangeParas.clear();
    for (size_t c = 0; c < m_3dImg->numChannels(); ++c) {
      m_sliceColormaps.emplace_back(
        std::make_unique<ZColorMapParameter>(QString("Slice Channel %1 Colormap").arg(c + 1)));
      m_sliceColormaps[c]->get().reset(
        0.0,
        1.0,
        QColor(0, 0, 0),
        QColor(m_3dImg->channelColor(c).r, m_3dImg->channelColor(c).g, m_3dImg->channelColor(c).b));
      m_doubleChannelRangeParas.emplace_back(
        std::make_unique<ZDoubleSpanParameter>(QString("Channel %1 Display Range").arg(c + 1),
                                               drs[c],
                                               packFor3D->rangeMin(),
                                               packFor3D->rangeMax()));
      m_doubleChannelRangeParas.back()->setStyle("SPINBOX");
      if (packFor3D->imgInfo().voxelFormat != VoxelFormat::Float) {
        m_doubleChannelRangeParas.back()->setDecimal(0);
        m_doubleChannelRangeParas.back()->setSingleStep(1);
      }
      connect(m_doubleChannelRangeParas[c].get(),
              &ZDoubleSpanParameter::valueChanged,
              this,
              &Z3DImgFilter::channelRangeChanged);
    }
    channelRangeChanged();

    bool is2DImage = m_3dImg->is2DData();
    glm::uvec3 volDim = m_3dImg->dimensions();
    m_xCut.setRange(0, volDim.x);
    m_xCut.set(m_xCut.range());
    m_yCut.setRange(0, volDim.y);
    m_yCut.set(m_yCut.range());
    m_zCut.setRange(0, volDim.z);
    m_zCut.set(m_zCut.range());

    m_obliqueSliceDistanceToOrigin.setRange(-glm::length(glm::vec3(volDim.x, volDim.y, volDim.z)),
                                            glm::length(glm::vec3(volDim.x, volDim.y, volDim.z)));
    m_obliqueSlice2DistanceToOrigin.setRange(-glm::length(glm::vec3(volDim.x, volDim.y, volDim.z)),
                                             glm::length(glm::vec3(volDim.x, volDim.y, volDim.z)));

    m_rendererParameters.coordTransform.setRotationCenter(glm::vec3(volDim.x, volDim.y, volDim.z) / 2.f);

    m_zSlicePosition.setRange(0, volDim.z - 1);
    m_ySlicePosition.setRange(0, volDim.y - 1);
    m_xSlicePosition.setRange(0, volDim.x - 1);
    m_zSlice2Position.setRange(0, volDim.z - 1);
    m_ySlice2Position.setRange(0, volDim.y - 1);
    m_xSlice2Position.setRange(0, volDim.x - 1);
    // invalidateAllFRVolumeSlices();
    // m_useFRVolumeSlice.set(!is2DImage);
    // m_useFRVolumeSlice.setVisible(!is2DImage);
    m_showXSlice.set(false);
    m_showYSlice.set(false);
    m_showZSlice.set(false);
    m_showObliqueSlice.set(false);
    m_showObliqueSlice2.set(false);
    m_showXSlice2.set(false);
    m_showYSlice2.set(false);
    m_showZSlice2.set(false);
    m_showXSlice.setVisible(!is2DImage);
    m_showYSlice.setVisible(!is2DImage);
    m_showZSlice.setVisible(!is2DImage);
    m_showObliqueSlice.setVisible(!is2DImage);
    m_showObliqueSlice2.setVisible(!is2DImage);
    m_showXSlice2.setVisible(!is2DImage);
    m_showYSlice2.setVisible(!is2DImage);
    m_showZSlice2.setVisible(!is2DImage);

    m_imgRaycasterRenderer.setData(*m_3dImg);

    std::vector<bool> channelVisibilities;
    channelVisibilities.reserve(m_3dImg->numChannels());
    std::vector<Z3DTransferFunction*> transferFunctions;
    transferFunctions.reserve(m_3dImg->numChannels());

    for (size_t c = 0; c < m_3dImg->numChannels(); ++c) {
      auto visiblePara = std::make_unique<ZBoolParameter>(QString("Show Channel %1").arg(c + 1), true);
      channelVisibilities.push_back(visiblePara->get());
      connect(visiblePara.get(), &ZBoolParameter::boolChanged, this, [this, c](bool value) {
        m_imgRaycasterRenderer.setChannelVisibility(c, value);
        invalidateResult();
      });
      m_channelVisibleParas.emplace_back(std::move(visiblePara));

      auto transferPara = std::make_unique<Z3DTransferFunctionParameter>(QString("Transfer Function %1").arg(c + 1));
      transferPara->setMinMaxIntensity(m_3dImg->displayRange(c).x, m_3dImg->displayRange(c).y);
      connect(transferPara.get(), &Z3DTransferFunctionParameter::valueChanged, this, [this]() {
        invalidateResult();
      });
      transferFunctions.push_back(&transferPara->get());

      auto& transferFunction = transferPara->get();
      transferFunction.reset(0.0,
                             1.0,
                             glm::vec4(0.f),
                             glm::vec4(m_3dImg->channelColor(c).r / 255.,
                                       m_3dImg->channelColor(c).g / 255.,
                                       m_3dImg->channelColor(c).b / 255.,
                                       1.f));
      transferFunction.captureDefaultFromCurrent();
      if (false) {
        transferFunction.addKey(ZColorMapKey(0.001, glm::vec4(0.01f, 0.01f, 0.01f, 0.0f)));
        transferFunction.addKey(ZColorMapKey(0.01, glm::vec4(0.01f, 0.01f, 0.01f, 1.0f)));
      }

      m_transferFuncParas.emplace_back(std::move(transferPara));
    }

    m_imgRaycasterRenderer.setChannelVisibilities(channelVisibilities);
    m_imgRaycasterRenderer.setTransferFunctions(transferFunctions);

    if (!is2DImage) {
      m_imgSliceRenderer.setData(*m_3dImg, m_sliceColormaps);
    }

    updateBoundBox();

    for (const auto& para : m_channelVisibleParas) {
      addParameter(*para);
    }
    for (const auto& para : m_doubleChannelRangeParas) {
      addParameter(*para);
    }
    for (const auto& para : m_transferFuncParas) {
      addParameter(*para);
    }
    for (const auto& cm : m_sliceColormaps) {
      addParameter(*cm);
    }

    if (m_widgetsGroup) {
      for (const auto& para : m_channelVisibleParas) {
        m_widgetsGroup->addChild(*para, 2);
      }
      for (const auto& para : m_doubleChannelRangeParas) {
        m_widgetsGroup->addChild(*para, 3);
      }
      for (const auto& para : m_transferFuncParas) {
        m_widgetsGroup->addChild(*para, 3);
      }
      for (const auto& cm : m_sliceColormaps) {
        m_widgetsGroup->addChild(*cm, 11);
      }
      m_widgetsGroup->emitWidgetsGroupChangedSignal();
    }

    connect(this, &Z3DImgFilter::showImgContextMenu, &imgPack, &ZImgPack::show3DImgContextMenu);
    connect(&imgPack, &ZImgPack::enterSubregionView, this, &Z3DImgFilter::enterSubregionView);
    connect(&imgPack, &ZImgPack::exitSubregionView, this, &Z3DImgFilter::exitSubregionView);
  }
  catch (const ZException& e) {
    m_3dImg.reset();
    LOG(ERROR) << e.what();
    Q_EMIT renderingError(QString("import 3d img error: %1").arg(e.what()));
  }
  catch (const std::exception& e) {
    m_3dImg.reset();
    LOG(ERROR) << e.what();
    Q_EMIT renderingError(QString("import 3d img error: %1").arg(e.what()));
  }

#ifdef NO // ATLAS_DEBUG_VERSION
  // Reset cached global cuts since our bounds may have changed with new data
  m_cachedGlobalCutsInitialized = false;
  debugSetInvalidateReason("setData");
#endif
  invalidateResult();
}

std::shared_ptr<ZWidgetsGroup> Z3DImgFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Img", 1);
    m_applyEmPresetButton = new QPushButton(QStringLiteral("Apply EM Preset"));
    m_applyEmPresetButton->setToolTip(
      QStringLiteral("Rewrite the current transfer functions and slice colormaps for EM-style grayscale rendering, "
                     "and switch the volume renderer to Direct Volume Rendering: "
                     "intensity 0 becomes transparent and non-zero values become opaque."));
    connect(m_applyEmPresetButton, &QPushButton::clicked, this, &Z3DImgFilter::applyEmVisualizationPreset);

    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_stayOnTop, 1);
    m_widgetsGroup->addChild(m_fullResolutionRendering, 1);
    // m_widgetsGroup->addChild(m_smoothInteraction, 1);

    for (const auto& para : m_channelVisibleParas) {
      m_widgetsGroup->addChild(*para, 2);
    }
    m_widgetsGroup->addChild(*m_applyEmPresetButton, 3);
    for (const auto& para : m_doubleChannelRangeParas) {
      m_widgetsGroup->addChild(*para, 3);
    }
    for (const auto& para : m_transferFuncParas) {
      m_widgetsGroup->addChild(*para, 3);
    }
    m_widgetsGroup->addChild(m_raycasterCompositingMode, 4);
    m_widgetsGroup->addChild(m_raycasterIsoValue, 4);
    m_widgetsGroup->addChild(m_raycasterLocalMIPThreshold, 4);
    m_widgetsGroup->addChild(m_raycasterSamplingRate, 15);

    m_widgetsGroup->addChild(m_xCut, 12);
    m_widgetsGroup->addChild(m_yCut, 12);
    m_widgetsGroup->addChild(m_zCut, 12);
    m_widgetsGroup->addChild(m_boundBoxMode, 13);
    m_widgetsGroup->addChild(m_boundBoxLineWidth, 13);
    m_widgetsGroup->addChild(m_boundBoxLineColor, 13);
    m_widgetsGroup->addChild(m_selectionLineWidth, 17);
    m_widgetsGroup->addChild(m_selectionLineColor, 17);
    m_widgetsGroup->addChild(m_manipulatorSize, 17);
    // m_widgetsGroup->addChild(m_interactionDownsample, 19);
    m_widgetsGroup->addChild(m_rendererParameters.coordTransform, 1);

    const std::vector<ZParameter*>& paras = parameters();
    for (auto para : paras) {
      if (para->name().contains("Slice") && !para->name().endsWith("2") && !para->name().endsWith("2 Position") &&
          !para->name().endsWith("2 Normal") && !para->name().endsWith("2 Distance to Origin")) {
        m_widgetsGroup->addChild(*para, 11);
      } else if (para->name().contains("Slice")) {
        m_widgetsGroup->addChild(*para, 19);
      }
    }
    m_widgetsGroup->setBasicAdvancedCutoff(14);
  }
  return m_widgetsGroup;
}

void Z3DImgFilter::applyEmVisualizationPreset()
{
  if (!m_3dImg) {
    return;
  }

  CHECK_EQ(m_transferFuncParas.size(), m_3dImg->numChannels())
    << "EM preset: transfer-function count does not match image channel count";
  CHECK_EQ(m_sliceColormaps.size(), m_3dImg->numChannels())
    << "EM preset: slice-colormap count does not match image channel count";

  for (size_t c = 0; c < m_3dImg->numChannels(); ++c) {
    const glm::col4 channelColor = emPresetOpaqueColor(m_3dImg->channelColor(c));
    m_transferFuncParas[c]->get().setKeys(emPresetKeys(m_transferFuncParas[c]->get().dimensions().x, channelColor));
    m_sliceColormaps[c]->get().setKeys(emPresetKeys(kSliceColormapLutWidth, channelColor));
  }

  m_raycasterCompositingMode.select(QStringLiteral("Direct Volume Rendering"));
}

bool Z3DImgFilter::isReady(Z3DEye eye) const
{
  return Z3DBoundedFilter::isReady(eye) && m_visible.get() && m_3dImg;
}

void Z3DImgFilter::emitPendingPagingWarning() const
{
  if (!m_3dImg) {
    return;
  }

  if (auto warning = m_3dImg->takePendingPagingWarning()) {
    Q_EMIT deferredRenderingWarning(QString::fromStdString(*warning));
  }
}

bool Z3DImgFilter::hasOpaque(Z3DEye) const
{
  return hasSlices();
}

void Z3DImgFilter::renderOpaque(Z3DEye eye)
{
  if (!m_opaqueValid[eye]) {
    return;
  }

  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    const auto& lease = m_opaqueTargets[eye];
    if (!lease || !lease.hasVulkanImage()) {
      return;
    }
    AttachmentHandle colorHandle;
    colorHandle.backend = RenderBackend::Vulkan;
    colorHandle.index = 0;
    colorHandle.id = reinterpret_cast<uint64_t>(lease.colorAttachment(0));

    AttachmentHandle depthHandle;
    depthHandle.backend = RenderBackend::Vulkan;
    depthHandle.index = 0;
    depthHandle.id = reinterpret_cast<uint64_t>(lease.depthAttachmentTexture());

    m_textureCopyRenderer.setSourceAttachments(colorHandle, depthHandle);
    m_rendererBase.renderVulkan(eye, m_textureCopyRenderer);
    return;
  }

  const auto& target = opaqueTarget(eye);
  m_textureCopyRenderer.setColorTexture(target.attachment(GL_COLOR_ATTACHMENT0));
  m_textureCopyRenderer.setDepthTexture(target.attachment(GL_DEPTH_ATTACHMENT));
  m_rendererBase.render(eye, m_textureCopyRenderer);
}

bool Z3DImgFilter::hasTransparent(Z3DEye eye) const
{
  return m_transparentValid[eye];
}

void Z3DImgFilter::renderTransparent(Z3DEye eye)
{
  if (!m_transparentValid[eye]) {
    return;
  }

  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    const auto& lease = m_transparentTargets[eye];
    if (!lease || !lease.hasVulkanImage()) {
      return;
    }
    AttachmentHandle colorHandle;
    colorHandle.backend = RenderBackend::Vulkan;
    colorHandle.index = 0;
    colorHandle.id = reinterpret_cast<uint64_t>(lease.colorAttachment(0));

    AttachmentHandle depthHandle;
    depthHandle.backend = RenderBackend::Vulkan;
    depthHandle.index = 0;
    depthHandle.id = reinterpret_cast<uint64_t>(lease.depthAttachmentTexture());

    m_textureCopyRenderer.setSourceAttachments(colorHandle, depthHandle);
    m_rendererBase.renderVulkan(eye, m_textureCopyRenderer);
    return;
  }

  const auto& target = transparentTarget(eye);
  m_textureCopyRenderer.setColorTexture(target.attachment(GL_COLOR_ATTACHMENT0));
  m_textureCopyRenderer.setDepthTexture(target.attachment(GL_DEPTH_ATTACHMENT));
  m_rendererBase.render(eye, m_textureCopyRenderer);
}

glm::vec3 Z3DImgFilter::get3DPosition(int x, int y, int width, int height, bool& success)
{
  const auto mode = static_cast<ImgCompositingMode>(m_raycasterCompositingMode.associatedData());

  if (mode == ImgCompositingMode::DirectVolumeRendering) {
    return getMaxInten3DPositionUnderScreenPoint(x, y, width, height, success);
  } else {
    return getFirstHit3DPosition(x, y, width, height, success);
  }
}

void Z3DImgFilter::setProgressiveRenderingMode(bool v)
{
  m_progressiveRendering = v;
  m_rendererBase.setCurrentRenderPassIsProgressive(v);
}

void Z3DImgFilter::enterSubregionView(float x, float y, float z)
{
  glm::vec3 pos3D(x, y, z);
  VLOG(1) << "open subregion at image coord " << pos3D;
  auto minCoord = pos3D - 64.f;
  auto maxCoord = pos3D + 64.f;
  m_xCut.set(glm::vec2(minCoord.x, maxCoord.x));
  m_yCut.set(glm::vec2(minCoord.y, maxCoord.y));
  m_zCut.set(glm::vec2(minCoord.z, maxCoord.z));
  m_globalParameters.cameraFocusesOn(axisAlignedBoundBoxAfterClipping());
  m_fullResolutionRendering.set(true);
}

void Z3DImgFilter::exitSubregionView()
{
  m_fullResolutionRendering.set(false);
  m_xCut.set(m_xCut.range());
  m_yCut.set(m_yCut.range());
  m_zCut.set(m_zCut.range());
  m_globalParameters.cameraFocusesOn(axisAlignedBoundBox());
}

void Z3DImgFilter::updateRaycasterSamplingRate()
{
  m_imgRaycasterRenderer.setSamplingRate(m_raycasterSamplingRate.get());
}

void Z3DImgFilter::updateRaycasterIsoValue()
{
  m_imgRaycasterRenderer.setIsoValue(m_raycasterIsoValue.get());
}

void Z3DImgFilter::updateRaycasterLocalMIPThreshold()
{
  m_imgRaycasterRenderer.setLocalMIPThreshold(m_raycasterLocalMIPThreshold.get());
}

void Z3DImgFilter::onVisibilityChanged(bool visible)
{
  if (visible) {
    return;
  }

  m_imgRaycasterRenderer.releaseScratchResources();
  for (Z3DEye eye : {MonoEye, LeftEye, RightEye}) {
    m_imgSliceRenderer.resetProgress(eye);
  }
  releaseAllRenderTargets();
}

void Z3DImgFilter::updateRaycasterCompositingMode()
{
  const auto mode = static_cast<ImgCompositingMode>(m_raycasterCompositingMode.associatedData());
  m_imgRaycasterRenderer.setCompositingMode(mode);

  const bool showIso = mode == ImgCompositingMode::IsoSurface;
  const bool showLocal = mode == ImgCompositingMode::LocalMIP || mode == ImgCompositingMode::LocalMIPOpaque;
  m_raycasterIsoValue.setVisible(showIso);
  m_raycasterLocalMIPThreshold.setVisible(showLocal);

  updateRaycasterIsoValue();
  updateRaycasterLocalMIPThreshold();
}

void Z3DImgFilter::invalidate(State inv)
{
  // Check for global cut churn that doesn't affect this image and skip invalidation if so.
#if 0 // ATLAS_DEBUG_VERSION
  QString reason = debugTakeInvalidateReason();
  if (!reason.isEmpty()) {
    if (reason.startsWith("global ")) {
      bool isCut =
        reason.contains("Global X Cut") || reason.contains("Global Y Cut") || reason.contains("Global Z Cut");
      if (isCut) {
        const auto& worldAABB = axisAlignedBoundBox();
        if (!worldAABB.empty()) {
          auto gx = m_globalParameters.globalXCut.get();
          auto gy = m_globalParameters.globalYCut.get();
          auto gz = m_globalParameters.globalZCut.get();
          glm::vec2 effX{std::clamp(gx[0], float(worldAABB.minCorner.x), float(worldAABB.maxCorner.x)),
                         std::clamp(gx[1], float(worldAABB.minCorner.x), float(worldAABB.maxCorner.x))};
          glm::vec2 effY{std::clamp(gy[0], float(worldAABB.minCorner.y), float(worldAABB.maxCorner.y)),
                         std::clamp(gy[1], float(worldAABB.minCorner.y), float(worldAABB.maxCorner.y))};
          glm::vec2 effZ{std::clamp(gz[0], float(worldAABB.minCorner.z), float(worldAABB.maxCorner.z)),
                         std::clamp(gz[1], float(worldAABB.minCorner.z), float(worldAABB.maxCorner.z))};
          if (effX.x > effX.y) {
            std::swap(effX.x, effX.y);
          }
          if (effY.x > effY.y) {
            std::swap(effY.x, effY.y);
          }
          if (effZ.x > effZ.y) {
            std::swap(effZ.x, effZ.y);
          }
          auto diffGt = [](const glm::vec2& a, const glm::vec2& b, float eps) {
            return std::abs(a.x - b.x) > eps || std::abs(a.y - b.y) > eps;
          };
          constexpr float eps = 1e-4f;
          bool changed = !m_cachedGlobalCutsInitialized || diffGt(effX, m_cachedEffXCut, eps) ||
                         diffGt(effY, m_cachedEffYCut, eps) || diffGt(effZ, m_cachedEffZCut, eps);
          if (!changed) {
            VLOG(1) << "skip invalidate: global cut changed but no effect on image AABB";
            return;
          }
          m_cachedGlobalCutsInitialized = true;
          m_cachedEffXCut = effX;
          m_cachedEffYCut = effY;
          m_cachedEffZCut = effZ;
        }
      }
    }
  }
#endif

  Z3DBoundedFilter::invalidate(inv);
  markTargetsInvalid();
  // If rendering is in progress, request cancellation; renderers will
  // catch cancellation and perform a safe reset of progressive state.
#if 0 // ATLAS_DEBUG_VERSION
  auto invStr = flagsToString(inv);
  auto stateStr = flagsToString(m_state);
  auto reason2 = reason;
  if (!reason2.isEmpty()) {
    VLOG(1) << "image filter invalidate: " << reason2 << ", inv=" << invStr << ", state=" << stateStr;
  } else {
    VLOG(1) << "image filter invalidate, inv=" << invStr << ", state=" << stateStr;
  }
#endif

  if (Z3DRenderGlobalState::instance().hasCancellationSource()) {
    Z3DRenderGlobalState::instance().requestCancellation();
#ifdef ATLAS_DEBUG_VERSION
    VLOG(1) << "requested cancellation on invalidate";
#endif
  }
  // Mark for safe reset at the beginning of next process
  m_resetProgressPending = true;
}

void Z3DImgFilter::updateSize(const glm::uvec2& targetSize)
{
  Z3DBoundedFilter::updateSize(targetSize);
  updateBlockIDTarget();

  if (targetSize.x == 0 || targetSize.y == 0) {
    return;
  }

  if (m_outputSize != targetSize) {
    m_outputSize = targetSize;
    releaseAllRenderTargets();
    setViewport(m_outputSize);
  }
}

void Z3DImgFilter::changeCoordTransform()
{
  VLOG(1) << "image coord changed";
  // invalidateAllFRVolumeSlices();
  if (m_3dImg) {
    m_3dImg->setScale(m_rendererParameters.coordTransform.scale());
  }
  // Coord transform changes do not affect shader headers for either the
  // raycaster or slice renderers (their macro headers depend on channel
  // visibility/count, compositing mode, and clip-plane presence/count).
  //
  // Rebuilding and relinking GLSL programs here is extremely expensive and can
  // dominate initial timeline application (e.g. setCurrentTime(0) during 3D
  // animation export), so keep this path side-effect-free.

#ifdef ATLAS_DEBUG_VERSION
  // World AABB changed; invalidate cached effective cuts
  m_cachedGlobalCutsInitialized = false;
#endif
}

void Z3DImgFilter::adjustWidget()
{
  m_zSlicePosition.setVisible(m_showZSlice.get());
  m_ySlicePosition.setVisible(m_showYSlice.get());
  m_xSlicePosition.setVisible(m_showXSlice.get());
  m_zSlice2Position.setVisible(m_showZSlice2.get());
  m_ySlice2Position.setVisible(m_showYSlice2.get());
  m_xSlice2Position.setVisible(m_showXSlice2.get());
  m_obliqueSliceNormal.setVisible(m_showObliqueSlice.get());
  m_obliqueSliceDistanceToOrigin.setVisible(m_showObliqueSlice.get());
  m_obliqueSlice2Normal.setVisible(m_showObliqueSlice2.get());
  m_obliqueSlice2DistanceToOrigin.setVisible(m_showObliqueSlice2.get());
}

void Z3DImgFilter::fullResolutionRenderingToggled()
{
  m_imgRaycasterRenderer.setFastRendering(!m_fullResolutionRendering.get());
  m_imgSliceRenderer.setFastRendering(!m_fullResolutionRendering.get());
  // m_smoothInteraction.setVisible(m_3dImg && m_3dImg->isVolumeDownsampled() && m_fullResolutionRendering.get());
}

void Z3DImgFilter::leftMouseButtonPressed(QMouseEvent* e, int w, int h)
{
  CHECK(e);
  if (!isVisible() || !m_3dImg) {
    return;
  }

  if (m_imgObjId == 0) {
    return;
  }

  if (e->type() == QEvent::MouseButtonPress) {
    m_startCoord.x = static_cast<int>(e->position().x());
    m_startCoord.y = static_cast<int>(e->position().y());
    return;
  }

  if (e->type() != QEvent::MouseButtonRelease) {
    return;
  }

  const int dx = std::abs(static_cast<int>(e->position().x()) - m_startCoord.x);
  const int dy = std::abs(static_cast<int>(e->position().y()) - m_startCoord.y);
  if (dx >= 2 || dy >= 2) {
    return;
  }

  // neuTube parity hook: expose the clicked 3D voxel (first-hit depth) to the UI thread.
  //
  // This is used by 3D SWC edit modes (Extend/Add-node/Connect-to) to match neuTube's
  // `Z3DVolumeFilter::pointInVolumeLeftClicked(...)` → `Z3DWindow::pointInVolumeLeftClicked(...)`
  // behavior. We intentionally do not gate this on the Trace tool toggle so SWC editing works
  // even when tracing is off.
  if (m_seedTraceSourceImgObjId.has_value() && *m_seedTraceSourceImgObjId == m_imgObjId) {
    if (m_seedTraceSourceChannel < m_channelVisibleParas.size()) {
      const auto& channelVisiblePara = m_channelVisibleParas[m_seedTraceSourceChannel];
      if (channelVisiblePara && channelVisiblePara->get()) {
        const glm::ivec2 widgetPos(e->position().toPoint().x(), e->position().toPoint().y());
        const void* hitObj = m_globalParameters.pickingManager.objectAtWidgetPos(widgetPos);
        if (hitObj == nullptr) {
          const float dpr = m_globalParameters.devicePixelRatio.get();
          bool success = false;
          const glm::vec3 pos3D = getFirstHit3DPosition(static_cast<int>(e->position().x() * dpr),
                                                        static_cast<int>(e->position().y() * dpr),
                                                        static_cast<int>(static_cast<float>(w) * dpr),
                                                        static_cast<int>(static_cast<float>(h) * dpr),
                                                        success);
          if (success) {
            Q_EMIT pointInVolumeLeftClicked(e->globalPosition().toPoint(),
                                            m_imgObjId,
                                            m_seedTraceSourceChannel,
                                            pos3D.x,
                                            pos3D.y,
                                            pos3D.z,
                                            e->modifiers());
          }
        }
      }
    }
  }

  if (!m_seedTraceToolEnabled || m_seedTraceInProgress) {
    return;
  }

  if (!m_seedTraceSourceImgObjId.has_value() || *m_seedTraceSourceImgObjId != m_imgObjId) {
    return;
  }

  if (m_seedTraceSourceChannel >= m_channelVisibleParas.size()) {
    return;
  }
  const auto& channelVisiblePara = m_channelVisibleParas[m_seedTraceSourceChannel];
  if (!channelVisiblePara || !channelVisiblePara->get()) {
    return;
  }

  const glm::ivec2 widgetPos(e->position().toPoint().x(), e->position().toPoint().y());
  const void* hitObj = m_globalParameters.pickingManager.objectAtWidgetPos(widgetPos);
  if (hitObj != nullptr) {
    return;
  }

  const float dpr = m_globalParameters.devicePixelRatio.get();
  bool success = false;
  const glm::vec3 pos3D = getMaxInten3DPositionUnderScreenPoint(static_cast<int>(e->position().x() * dpr),
                                                                static_cast<int>(e->position().y() * dpr),
                                                                static_cast<int>(static_cast<float>(w) * dpr),
                                                                static_cast<int>(static_cast<float>(h) * dpr),
                                                                success);
  if (!success) {
    return;
  }

  e->accept();
  Q_EMIT showSeedTraceContextMenu(e->globalPosition().toPoint(),
                                  m_imgObjId,
                                  m_seedTraceSourceChannel,
                                  pos3D.x,
                                  pos3D.y,
                                  pos3D.z);
}

void Z3DImgFilter::contextMenuEvent(QContextMenuEvent* event, int w, int h)
{
  if (isVisible() && isSelected() && m_3dImg) {
    bool success = false;
    auto pos3D = get3DPosition(event->x() * m_globalParameters.devicePixelRatio.get(),
                               event->y() * m_globalParameters.devicePixelRatio.get(),
                               w * m_globalParameters.devicePixelRatio.get(),
                               h * m_globalParameters.devicePixelRatio.get(),
                               success);

    bool enter = success;
    bool exit = m_xCut.get() != m_xCut.range() || m_yCut.get() != m_yCut.range() || m_zCut.get() != m_zCut.range();
    if (!enter && !exit) {
      return;
    }

    Q_EMIT showImgContextMenu(event->globalPos(), pos3D.x, pos3D.y, pos3D.z, enter, exit);
  }
}

// void Z3DImgFilter::invalidateFRVolumeZSlice()
//{
//   m_FRVolumeSlicesValidState[0] = false;
// }

// void Z3DImgFilter::invalidateFRVolumeYSlice()
//{
//   m_FRVolumeSlicesValidState[1] = false;
// }

// void Z3DImgFilter::invalidateFRVolumeXSlice()
//{
//   m_FRVolumeSlicesValidState[2] = false;
// }

// void Z3DImgFilter::invalidateFRVolumeZSlice2()
//{
//   m_FRVolumeSlicesValidState[3] = false;
// }

// void Z3DImgFilter::invalidateFRVolumeYSlice2()
//{
//   m_FRVolumeSlicesValidState[4] = false;
// }

// void Z3DImgFilter::invalidateFRVolumeXSlice2()
//{
//   m_FRVolumeSlicesValidState[5] = false;
// }

// void Z3DImgFilter::enterFastMode()
//{
//   if (m_smoothInteraction.get() && m_3dImg && m_3dImg->isVolumeDownsampled() && m_fullResolutionRendering.get()) {
//     m_imgRaycasterRenderer.setFastRendering(true);
//     m_imgSliceRenderer.setFastRendering(true);
//   }
// }
//
// void Z3DImgFilter::exitFastMode()
//{
//   if (m_smoothInteraction.get() && m_3dImg && m_3dImg->isVolumeDownsampled() && m_fullResolutionRendering.get()) {
//     m_imgRaycasterRenderer.setFastRendering(false);
//     m_imgSliceRenderer.setFastRendering(false);
//     // upstream will invalidate the network, but in case there are no upstream
//     // do one more invalidation
//     if (m_imgRaycasterRenderer.lastRenderingIsFastRendering() || m_imgSliceRenderer.lastRenderingIsFastRendering()) {
//       invalidateResult();
//     }
//   }
// }

double Z3DImgFilter::process(Z3DEye eye)
{
  syncRendererState();
  // VLOG(2) << "state synced " << m_rendererBase.frameState().viewport;

  if (m_3dImg) {
    m_3dImg->clearPendingPagingWarnings();
  }

  // Apply any deferred progressive reset at a safe point
  if (m_resetProgressPending) {
    m_imgRaycasterRenderer.resetProgress(MonoEye);
    m_imgRaycasterRenderer.resetProgress(LeftEye);
    m_imgRaycasterRenderer.resetProgress(RightEye);
    m_imgSliceRenderer.resetProgress(MonoEye);
    m_imgSliceRenderer.resetProgress(LeftEye);
    m_imgSliceRenderer.resetProgress(RightEye);
    m_resetProgressPending = false;
  }

  if (m_channelRangeChanged) {
    if (m_3dImg) {
      std::vector<glm::dvec2> channelDisplayRanges;
      for (const auto& para : m_doubleChannelRangeParas) {
        channelDisplayRanges.push_back(para->get());
      }
      m_3dImg->setChannelDisplayRanges(channelDisplayRanges);
      if (auto warning = m_3dImg->takePendingPreviewWarning()) {
        Q_EMIT deferredRenderingWarning(QString::fromStdString(*warning));
      }
      for (size_t i = 0; i < m_transferFuncParas.size() && i < m_3dImg->numChannels(); ++i) {
        auto channelImage = m_3dImg->channelImageShared(i);
        m_transferFuncParas[i]->setImage(std::move(channelImage));
        m_transferFuncParas[i]->setMinMaxIntensity(m_3dImg->displayRange(i).x, m_3dImg->displayRange(i).y);
      }
    }

    m_channelRangeChanged = false;
  }

  const bool isVulkan = (m_rendererBase.activeBackend() == RenderBackend::Vulkan);

  // ---------------------------------------------------------------------------
  // Vulkan: express raycaster + slice pipelines via linear script (GL-like order,
  // explicit segments, backend-owned submission boundaries).
  // ---------------------------------------------------------------------------
  if (isVulkan) {
    auto* vulkanBackend = dynamic_cast<Z3DRendererVulkanBackend*>(m_rendererBase.backend());
    CHECK(vulkanBackend != nullptr) << "ImgFilter Vulkan path requires a Vulkan backend";
    CHECK(!m_rendererBase.isVulkanFrameActive())
      << "ImgFilter Vulkan process must not run inside an active Vulkan frame (script owns submission boundaries)";

    const bool doImage = hasImage();
    const bool doBBoxOnly = (!doImage && onlyBoundBox());
    const bool doSlices = hasSlices();

    // Nothing to render: keep validity flags untouched and report done.
    if (!doImage && !doBBoxOnly && !doSlices) {
      return 1.0;
    }

    const bool blockImageCaptureToCompletion = doImage && !m_progressiveRendering && m_3dImg &&
                                               m_3dImg->isVolumeDownsampled() &&
                                               !m_imgRaycasterRenderer.isFastRendering();
    const bool blockSliceCaptureToCompletion = doSlices && !m_progressiveRendering && m_3dImg &&
                                               m_3dImg->isVolumeDownsampled() && !m_imgSliceRenderer.isFastRendering();
    const bool blockCaptureToCompletion = blockImageCaptureToCompletion || blockSliceCaptureToCompletion;
    const bool useProgressiveImage = m_progressiveRendering || blockImageCaptureToCompletion;
    const bool useProgressiveSlices = m_progressiveRendering || blockSliceCaptureToCompletion;

    const char* eyeTag = (eye == MonoEye) ? "mono" : (eye == LeftEye) ? "left" : "right";
    const std::string frameLabel = std::string("img_filter_") + eyeTag;

    bool imagePassComplete = !doImage;
    bool transparentPassComplete = !(doImage || doBBoxOnly);
    bool slicePassComplete = !doSlices;

    const auto runOneVulkanPass = [&](bool runTransparentPass, bool runSlicePass) {
      double imageProgress = imagePassComplete ? 1.0 : 0.0;
      double slicesProgress = slicePassComplete ? 1.0 : 0.0;

      // Prepare persistent per-eye output leases once; the script nodes will
      // record into these surfaces.
      Z3DScratchResourcePool::RenderTargetLease* transparentLease = nullptr;
      Z3DScratchResourcePool::RenderTargetLease* opaqueLease = nullptr;

      if (runTransparentPass) {
        auto& lease = m_transparentTargets[eye];
        if (!lease || lease.descriptor.size != m_outputSize || !lease.hasVulkanImage()) {
          lease.release();
          m_rendererBase.acquirePersistentTempRenderTarget2D(lease,
                                                             m_outputSize,
                                                             ScratchFormat::RGBA16,
                                                             ScratchFormat::Depth32F);
        }
        transparentLease = &lease;
      }
      if (runSlicePass) {
        auto& lease = m_opaqueTargets[eye];
        if (!lease || lease.descriptor.size != m_outputSize || !lease.hasVulkanImage()) {
          lease.release();
          m_rendererBase.acquirePersistentTempRenderTarget2D(lease,
                                                             m_outputSize,
                                                             ScratchFormat::RGBA16,
                                                             ScratchFormat::Depth32F);
        }
        opaqueLease = &lease;
      }

      try {
        ZVulkanLinearScript script(m_rendererBase, *vulkanBackend, frameLabel);

        ZVulkanLinearScript::SegmentHandle segRaycaster{};

        if (runTransparentPass && doImage) {
          // Geometry/state prep only on the first step of a progressive cycle.
          // For blocking capture/export, reuse the same progressive machinery
          // so Vulkan reaches the same full-resolution state as OpenGL before
          // returning to the caller.
          const bool progressiveStep = (useProgressiveImage && m_imgRaycasterRenderer.renderingStarted(eye));
          if (!progressiveStep) {
            prepareRaycasterInputs(eye, m_outputSize);
          }

          CHECK(transparentLease != nullptr) << "ImgFilter Vulkan image path missing transparent lease";

          segRaycaster =
            m_imgRaycasterRenderer.recordVulkanStagesToScript(script,
                                                              eye,
                                                              *transparentLease,
                                                              {},
                                                              /*interactiveProgressivePaging=*/m_progressiveRendering);

          auto recordBBox = [&]() {
            const auto prevViewport = m_rendererBase.frameState().viewport;
            const auto prevSurface = m_rendererBase.frameState().activeSurface;
            auto guard = folly::makeGuard([&]() {
              m_rendererBase.frameState().updateViewportData(prevViewport);
              m_rendererBase.setActiveSurfaceWithLoadStore(prevSurface, Z3DRendererBase::Preserve);
            });

            m_rendererBase.setActiveSurfaceWithLoadStore(*transparentLease, Z3DRendererBase::Preserve);
            for (auto& att : m_rendererBase.frameState().activeSurface.colorAttachments) {
              att.finalUse = AttachmentFinalUse::Sampled;
            }
            if (m_rendererBase.frameState().activeSurface.depthAttachment) {
              m_rendererBase.frameState().activeSurface.depthAttachment->finalUse = AttachmentFinalUse::Sampled;
            }
            renderBoundBox(eye, Z3DBoundedFilter::BoundBoxRenderStyle::OverlayAlphaDepth);
          };
          if (segRaycaster) {
            script.raster("bbox_overlay", {segRaycaster}, recordBBox);
          } else {
            script.raster("bbox_overlay", {}, recordBBox);
          }
        } else if (runTransparentPass && doBBoxOnly) {
          CHECK(transparentLease != nullptr) << "ImgFilter Vulkan bbox-only path missing transparent lease";
          script.raster("bbox_overlay", {}, [&]() {
            const auto prevViewport = m_rendererBase.frameState().viewport;
            const auto prevSurface = m_rendererBase.frameState().activeSurface;
            auto guard = folly::makeGuard([&]() {
              m_rendererBase.frameState().updateViewportData(prevViewport);
              m_rendererBase.setActiveSurfaceWithLoadStore(prevSurface, Z3DRendererBase::Preserve);
            });

            m_rendererBase.setActiveSurfaceWithLoadStore(*transparentLease,
                                                         LoadOp::Clear,
                                                         StoreOp::Store,
                                                         LoadOp::Clear,
                                                         StoreOp::Store);
            for (auto& att : m_rendererBase.frameState().activeSurface.colorAttachments) {
              att.finalUse = AttachmentFinalUse::Sampled;
            }
            if (m_rendererBase.frameState().activeSurface.depthAttachment) {
              m_rendererBase.frameState().activeSurface.depthAttachment->finalUse = AttachmentFinalUse::Sampled;
            }

            renderBoundBox(eye, Z3DBoundedFilter::BoundBoxRenderStyle::OverlayAlphaDepth);
          });
        }

        if (runSlicePass) {
          const bool progressiveStep = (useProgressiveSlices && m_imgSliceRenderer.renderingStarted(eye));
          if (!progressiveStep) {
            prepareSliceInputs(eye, m_outputSize);
          }

          CHECK(opaqueLease != nullptr) << "ImgFilter Vulkan slice path missing opaque lease";
          [[maybe_unused]] const auto segSlices =
            m_imgSliceRenderer.recordVulkanStagesToScript(script, eye, *opaqueLease);
        }

        script.flush("imgfilter_done");
      }
      catch (const ZCancellationException&) {
        if (doImage) {
          m_imgRaycasterRenderer.resetProgress(eye);
        }
        if (doSlices) {
          m_imgSliceRenderer.resetProgress(eye);
        }
        throw;
      }
      catch (const folly::OperationCancelled&) {
        if (doImage) {
          m_imgRaycasterRenderer.resetProgress(eye);
        }
        if (doSlices) {
          m_imgSliceRenderer.resetProgress(eye);
        }
        throw;
      }

      double currentProgress = 0.0;
      double totalProgress = 0.0;
      if (doImage) {
        if (runTransparentPass) {
          imageProgress = useProgressiveImage ? m_imgRaycasterRenderer.progressiveProgress(eye) : 1.0;
        }
        currentProgress += imageProgress;
        totalProgress += 1.0;

        if (useProgressiveImage && imageProgress >= 1.0) {
          m_imgRaycasterRenderer.finalizePagingStatsIfDone(eye);
        }
        if (!useProgressiveImage || imageProgress >= 1.0) {
          m_imgRaycasterRenderer.releaseEntryExit();
          imagePassComplete = true;
        }
        m_transparentValid[eye] = true;
      } else if (doBBoxOnly) {
        m_transparentValid[eye] = true;
      }
      if (runTransparentPass) {
        transparentPassComplete = true;
      }

      if (doSlices) {
        if (runSlicePass) {
          slicesProgress = useProgressiveSlices ? m_imgSliceRenderer.progressiveProgress(eye) : 1.0;
        }
        currentProgress += slicesProgress;
        totalProgress += 1.0;
        if (!useProgressiveSlices || slicesProgress >= 1.0) {
          slicePassComplete = true;
        }
        m_opaqueValid[eye] = true;
      }

      emitPendingPagingWarning();

      const double overallProgress = totalProgress > 0.0 ? currentProgress / totalProgress : 1.0;
      if (!m_progressiveRendering && !blockCaptureToCompletion) {
        CHECK(currentProgress == totalProgress) << currentProgress << " " << totalProgress;
      }
      return overallProgress;
    };

    double overallProgress = 0.0;
    do {
      const bool runTransparentPass = !transparentPassComplete || !imagePassComplete;
      const bool runSlicePass = !slicePassComplete;
      overallProgress = runOneVulkanPass(runTransparentPass, runSlicePass);
    } while (blockCaptureToCompletion && overallProgress < 1.0);

    return overallProgress;
  }

  // ---------------------------------------------------------------------------
  // OpenGL: existing immediate path.
  // ---------------------------------------------------------------------------
  double currentProgress = 0.0;
  double totalProgress = 0.0;

  if (hasImage()) {
    double progress = renderImage(eye);
    currentProgress += progress;
    totalProgress += 1.0;
  } else if (onlyBoundBox()) {
    renderOnlyBoundBox(eye);
  }

  if (hasSlices()) {
    double progress = renderSlices(eye);
    currentProgress += progress;
    totalProgress += 1.0;
  }

  emitPendingPagingWarning();

  CHECK_GL_ERROR

  if (!m_progressiveRendering) {
    CHECK(currentProgress == totalProgress) << currentProgress << " " << totalProgress;
  }
  return totalProgress > 0 ? currentProgress / totalProgress : 1.0;
}

bool Z3DImgFilter::hasSlices() const
{
  return m_showZSlice.get() || m_showXSlice.get() || m_showYSlice.get() || m_showXSlice2.get() || m_showYSlice2.get() ||
         m_showZSlice2.get() || m_showObliqueSlice.get() || m_showObliqueSlice2.get();
}

void Z3DImgFilter::prepareSliceInputs(Z3DEye /*eye*/, const glm::uvec2& outputSize)
{
  CHECK(m_3dImg) << "prepareSliceInputs requires a valid image";
  m_imgSliceRenderer.setOutputSize(outputSize);

  glm::uvec3 volDim = glm::max(glm::uvec3(2, 2, 2), m_3dImg->dimensions());
  glm::vec3 coordLuf = m_3dImg->physicalLUF();
  glm::vec3 coordRdb = m_3dImg->physicalRDB();

  m_imgSliceRenderer.clearSlices();

  ZMesh cube = ZMesh::createCube(coordLuf, coordRdb, glm::vec3(0, 0, 0), glm::vec3(1, 1, 1));
  if (m_showObliqueSlice.get()) {
    glm::vec3 normal = m_obliqueSliceNormal.get();
    if (glm::length(normal) == 0) {
      normal = glm::vec3(1, 1, 0);
    } else {
      normal = glm::normalize(normal);
    }
    ZMesh slice = ZMesh::planeClosedSurfaceIntersection(cube, normal, m_obliqueSliceDistanceToOrigin.get() * normal);
    if (!slice.empty()) {
      slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
      m_imgSliceRenderer.addSlice(slice);
    }
  }
  if (m_showZSlice.get()) {
    float zTexCoord = (m_zSlicePosition.get() + .5f) / volDim.z;
    float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);
    ZMesh slice = ZMesh::createCubeSlice(zCoord, zTexCoord, 2, coordLuf.xy(), coordRdb.xy());
    slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
    m_imgSliceRenderer.addSlice(slice);
  }
  if (m_showYSlice.get()) {
    float yTexCoord = (m_ySlicePosition.get() + .5f) / volDim.y;
    float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);
    ZMesh slice = ZMesh::createCubeSlice(yCoord, yTexCoord, 1, coordLuf.xz(), coordRdb.xz());
    slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
    m_imgSliceRenderer.addSlice(slice);
  }
  if (m_showXSlice.get()) {
    float xTexCoord = (m_xSlicePosition.get() + .5f) / volDim.x;
    float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);
    ZMesh slice = ZMesh::createCubeSlice(xCoord, xTexCoord, 0, coordLuf.yz(), coordRdb.yz());
    slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
    m_imgSliceRenderer.addSlice(slice);
  }
  if (m_showObliqueSlice2.get()) {
    glm::vec3 normal = m_obliqueSlice2Normal.get();
    if (glm::length(normal) == 0) {
      normal = glm::vec3(1, 1, 0);
    } else {
      normal = glm::normalize(normal);
    }
    ZMesh slice = ZMesh::planeClosedSurfaceIntersection(cube, normal, m_obliqueSlice2DistanceToOrigin.get() * normal);
    if (!slice.empty()) {
      slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
      m_imgSliceRenderer.addSlice(slice);
    }
  }
  if (m_showZSlice2.get()) {
    float zTexCoord = (m_zSlice2Position.get() + .5f) / volDim.z;
    float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);
    ZMesh slice = ZMesh::createCubeSlice(zCoord, zTexCoord, 2, coordLuf.xy(), coordRdb.xy());
    slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
    m_imgSliceRenderer.addSlice(slice);
  }
  if (m_showYSlice2.get()) {
    float yTexCoord = (m_ySlice2Position.get() + .5f) / volDim.y;
    float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);
    ZMesh slice = ZMesh::createCubeSlice(yCoord, yTexCoord, 1, coordLuf.xz(), coordRdb.xz());
    slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
    m_imgSliceRenderer.addSlice(slice);
  }
  if (m_showXSlice2.get()) {
    float xTexCoord = (m_xSlice2Position.get() + .5f) / volDim.x;
    float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);
    ZMesh slice = ZMesh::createCubeSlice(xCoord, xTexCoord, 0, coordLuf.yz(), coordRdb.yz());
    slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
    m_imgSliceRenderer.addSlice(slice);
  }
}

double Z3DImgFilter::renderSlices(Z3DEye eye)
{
  CHECK(m_rendererBase.activeBackend() != RenderBackend::Vulkan)
    << "Z3DImgFilter::renderSlices is OpenGL-only; Vulkan uses ZVulkanLinearScript in process().";

  Z3DRenderTarget& currentTarget = opaqueTarget(eye);

  const bool progressiveStep = m_progressiveRendering && m_imgSliceRenderer.renderingStarted(eye);
  if (!progressiveStep) {
    prepareSliceInputs(eye, currentTarget.size());
  }

  currentTarget.bind();
  currentTarget.clear();
  setViewport(currentTarget.size());

  m_opaqueValid[eye] = false;

  auto targetGuard = folly::makeGuard([&currentTarget, this, eye]() {
    currentTarget.release();
    m_opaqueValid[eye] = true;
  });

  double progress = 1.0;
  if (!m_progressiveRendering) {
    m_rendererBase.render(eye, m_imgSliceRenderer);
  } else {
    progress = m_imgSliceRenderer.renderProgressively(eye);
  }

  return progress;
}

bool Z3DImgFilter::hasImage() const
{
  return m_imgRaycasterRenderer.hasVisibleRendering() && m_xCut.upperValue() > m_xCut.minimum() &&
         m_yCut.upperValue() > m_yCut.minimum() && m_zCut.upperValue() > m_zCut.minimum() &&
         m_xCut.lowerValue() < m_xCut.maximum() && m_yCut.lowerValue() < m_yCut.maximum() &&
         m_zCut.lowerValue() < m_zCut.maximum();
}

bool Z3DImgFilter::saveRawMIPImage(const QString& path, std::string& error)
{
  if (!FLAGS_atlas_enable_benchmark_raw_mip_export) {
    error = "raw MIP export is disabled; relaunch Atlas with --atlas_enable_benchmark_raw_mip_export";
    return false;
  }
  if (m_rendererBase.activeBackend() != RenderBackend::OpenGL) {
    error = "raw MIP export is currently only supported for the OpenGL backend";
    return false;
  }
  if (!m_3dImg) {
    error = "raw MIP export requires an active image";
    return false;
  }
  if (!hasImage()) {
    error = "raw MIP export requires a visible volume render";
    return false;
  }

  Z3DRenderTarget& currentTarget = transparentTarget(MonoEye);
  m_imgRaycasterRenderer.resetProgress(MonoEye);
  prepareRaycasterInputs(MonoEye, currentTarget.size());
  const bool ok = m_imgRaycasterRenderer.saveRawMIPImage(MonoEye, path, error);
  m_imgRaycasterRenderer.resetProgress(MonoEye);
  return ok;
}

bool Z3DImgFilter::screenSpaceSufficiencyAudit(ScreenSpaceSufficiencyAudit& audit, std::string& error)
{
  if (!FLAGS_atlas_enable_benchmark_screen_space_sufficiency_audit) {
    error = "screen-space audit is disabled; relaunch Atlas with "
            "--atlas_enable_benchmark_screen_space_sufficiency_audit";
    return false;
  }
  if (m_rendererBase.activeBackend() != RenderBackend::OpenGL) {
    error = "screen-space audit is currently only supported for the OpenGL backend";
    return false;
  }
  if (!m_3dImg) {
    error = "screen-space audit requires an active image";
    return false;
  }
  if (!hasImage()) {
    error = "screen-space audit requires a visible volume render";
    return false;
  }

  Z3DRenderTarget& currentTarget = transparentTarget(MonoEye);
  m_imgRaycasterRenderer.resetProgress(MonoEye);
  prepareRaycasterInputs(MonoEye, currentTarget.size());
  const glm::vec3 scale = glm::abs(m_rendererParameters.coordTransform.scale());
  const float selectedVoxelWorldSize = std::min(std::min(scale.x, scale.y), scale.z);
  CHECK(std::isfinite(selectedVoxelWorldSize) && selectedVoxelWorldSize > 0.0f) << selectedVoxelWorldSize;
  m_imgRaycasterRenderer.setBenchmarkSelectedVoxelWorldSizeHint(selectedVoxelWorldSize);
  const bool ok = m_imgRaycasterRenderer.screenSpaceSufficiencyAudit(MonoEye, audit, error);
  m_imgRaycasterRenderer.resetProgress(MonoEye);
  return ok;
}

void Z3DImgFilter::prepareRaycasterInputs(Z3DEye eye, const glm::uvec2& outputSize)
{
  CHECK(m_3dImg) << "prepareRaycasterInputs requires a valid image";

  m_imgRaycasterRenderer.setOutputSize(outputSize);

  glm::uvec3 volDim = glm::max(glm::uvec3(2, 2, 2), m_3dImg->dimensions());
  glm::vec3 coordLuf = m_3dImg->physicalLUF();
  glm::vec3 coordRdb = m_3dImg->physicalRDB();

  float xTexCoordStart = m_xCut.lowerValue() / volDim.x;
  float xTexCoordEnd = m_xCut.upperValue() / volDim.x;
  float xCoordStart = glm::mix(coordLuf.x, coordRdb.x, xTexCoordStart);
  float xCoordEnd = glm::mix(coordLuf.x, coordRdb.x, xTexCoordEnd);
  float yTexCoordStart = m_yCut.lowerValue() / volDim.y;
  float yTexCoordEnd = m_yCut.upperValue() / volDim.y;
  float yCoordStart = glm::mix(coordLuf.y, coordRdb.y, yTexCoordStart);
  float yCoordEnd = glm::mix(coordLuf.y, coordRdb.y, yTexCoordEnd);
  float zTexCoordStart = m_zCut.lowerValue() / volDim.z;
  float zTexCoordEnd = m_zCut.upperValue() / volDim.z;
  float zCoordStart = glm::mix(coordLuf.z, coordRdb.z, zTexCoordStart);
  float zCoordEnd = glm::mix(coordLuf.z, coordRdb.z, zTexCoordEnd);

  if (m_3dImg->is2DData()) { // for 2d image
    m_imgRaycasterRenderer.clearAnalyticRaySetup();
    m_imgRaycasterRenderer.releaseEntryExit();
    ZMesh quad = ZMesh::createImageSlice(0,
                                         glm::vec2(xCoordStart, yCoordStart),
                                         glm::vec2(xCoordEnd, yCoordEnd),
                                         glm::vec2(xTexCoordStart, yTexCoordStart),
                                         glm::vec2(xTexCoordEnd, yTexCoordEnd));
    quad.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
    m_imgRaycasterRenderer.clearQuads();
    m_imgRaycasterRenderer.addQuad(quad);
    return;
  }

  if (m_zCut.lowerValue() == m_zCut.upperValue()) { // slice of 3d image
    m_imgRaycasterRenderer.clearAnalyticRaySetup();
    m_imgRaycasterRenderer.releaseEntryExit();
    ZMesh quad = ZMesh::createCubeSlice(zCoordStart,
                                        zTexCoordStart,
                                        2,
                                        glm::vec2(xCoordStart, yCoordStart),
                                        glm::vec2(xCoordEnd, yCoordEnd),
                                        glm::vec2(xTexCoordStart, yTexCoordStart),
                                        glm::vec2(xTexCoordEnd, yTexCoordEnd));
    quad.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
    m_imgRaycasterRenderer.clearQuads();
    m_imgRaycasterRenderer.addQuad(quad);
    return;
  }

  if (m_yCut.lowerValue() == m_yCut.upperValue()) { // slice of 3d image
    m_imgRaycasterRenderer.clearAnalyticRaySetup();
    m_imgRaycasterRenderer.releaseEntryExit();
    ZMesh quad = ZMesh::createCubeSlice(yCoordStart,
                                        yTexCoordStart,
                                        1,
                                        glm::vec2(xCoordStart, zCoordStart),
                                        glm::vec2(xCoordEnd, zCoordEnd),
                                        glm::vec2(xTexCoordStart, zTexCoordStart),
                                        glm::vec2(xTexCoordEnd, zTexCoordEnd));
    quad.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
    m_imgRaycasterRenderer.clearQuads();
    m_imgRaycasterRenderer.addQuad(quad);
    return;
  }

  if (m_xCut.lowerValue() == m_xCut.upperValue()) { // slice of 3d image
    m_imgRaycasterRenderer.clearAnalyticRaySetup();
    m_imgRaycasterRenderer.releaseEntryExit();
    ZMesh quad = ZMesh::createCubeSlice(xCoordStart,
                                        xTexCoordStart,
                                        0,
                                        glm::vec2(yCoordStart, zCoordStart),
                                        glm::vec2(yCoordEnd, zCoordEnd),
                                        glm::vec2(yTexCoordStart, zTexCoordStart),
                                        glm::vec2(yTexCoordEnd, zTexCoordEnd));
    quad.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
    m_imgRaycasterRenderer.clearQuads();
    m_imgRaycasterRenderer.addQuad(quad);
    return;
  }

  // Keep viewport state consistent for subsequent batch capture.
  setViewport(outputSize);
  if (m_rendererBase.activeBackend() != RenderBackend::Vulkan) {
    CHECK_GL_ERROR
  }

  const glm::mat4 coordTransform = m_rendererParameters.coordTransform.get();

  m_imgRaycasterRenderer.clearQuads();

  if (FLAGS_atlas_volume_rendering_analytic_ray_setup) {
    const float coordDeterminant = glm::determinant(glm::mat3(coordTransform));
    CHECK(std::abs(coordDeterminant) > 1e-8f) << "Analytic ray setup requires an invertible coordTransform";

    Z3DAnalyticRaySetup setup;
    setup.enabled = true;
    setup.boxMinTex = glm::min(glm::vec3(xTexCoordStart, yTexCoordStart, zTexCoordStart),
                               glm::vec3(xTexCoordEnd, yTexCoordEnd, zTexCoordEnd));
    setup.boxMaxTex = glm::max(glm::vec3(xTexCoordStart, yTexCoordStart, zTexCoordStart),
                               glm::vec3(xTexCoordEnd, yTexCoordEnd, zTexCoordEnd));
    setup.ndcZRange =
      (m_rendererBase.activeBackend() == RenderBackend::Vulkan) ? glm::vec2(0.0f, 1.0f) : glm::vec2(-1.0f, 1.0f);

    const glm::mat4 worldToTex = localToTexMatrix(coordLuf, coordRdb) * glm::inverse(coordTransform);
    const glm::mat4 fragCoordToNdc = fragCoordToNdcMatrix(outputSize);
    const auto& eyeState = m_rendererBase.viewState().eyes[eye];
    setup.ndcToTex = worldToTex * eyeState.inverseViewMatrix * eyeState.inverseProjectionMatrix * fragCoordToNdc;
    setup.ndcToEye = eyeState.inverseProjectionMatrix * fragCoordToNdc;

    std::array<glm::vec4, kZ3DAnalyticRaySetupMaxClipPlanes> worldPlanes{};
    uint32_t worldPlaneCount = 0u;
    auto appendWorldPlane = [&](const glm::vec4& plane) {
      CHECK_LT(worldPlaneCount, static_cast<uint32_t>(worldPlanes.size()))
        << "Analytic ray setup exceeded the supported global cut plane budget";
      worldPlanes[worldPlaneCount++] = plane;
    };

    if (m_globalParameters.globalXCut.lowerValue() != m_globalParameters.globalXCut.minimum()) {
      appendWorldPlane(glm::vec4(1.0f, 0.0f, 0.0f, -m_globalParameters.globalXCut.lowerValue()));
    }
    if (m_globalParameters.globalXCut.upperValue() != m_globalParameters.globalXCut.maximum()) {
      appendWorldPlane(glm::vec4(-1.0f, 0.0f, 0.0f, m_globalParameters.globalXCut.upperValue()));
    }
    if (m_globalParameters.globalYCut.lowerValue() != m_globalParameters.globalYCut.minimum()) {
      appendWorldPlane(glm::vec4(0.0f, 1.0f, 0.0f, -m_globalParameters.globalYCut.lowerValue()));
    }
    if (m_globalParameters.globalYCut.upperValue() != m_globalParameters.globalYCut.maximum()) {
      appendWorldPlane(glm::vec4(0.0f, -1.0f, 0.0f, m_globalParameters.globalYCut.upperValue()));
    }
    if (m_globalParameters.globalZCut.lowerValue() != m_globalParameters.globalZCut.minimum()) {
      appendWorldPlane(glm::vec4(0.0f, 0.0f, 1.0f, -m_globalParameters.globalZCut.lowerValue()));
    }
    if (m_globalParameters.globalZCut.upperValue() != m_globalParameters.globalZCut.maximum()) {
      appendWorldPlane(glm::vec4(0.0f, 0.0f, -1.0f, m_globalParameters.globalZCut.upperValue()));
    }

    const glm::mat4 planeTransform = glm::transpose(glm::inverse(worldToTex));
    for (uint32_t i = 0; i < worldPlaneCount; ++i) {
      glm::vec4 texPlane = planeTransform * worldPlanes[i];
      const float normalLength = glm::length(glm::vec3(texPlane));
      CHECK_GT(normalLength, 1e-6f) << "Analytic ray setup produced a degenerate texture-space clip plane";
      setup.clipPlanes[setup.clipPlaneCount++] = texPlane / normalLength;
    }

    m_imgRaycasterRenderer.releaseEntryExit();
    m_imgRaycasterRenderer.setAnalyticRaySetup(setup);
    return;
  }

  m_imgRaycasterRenderer.clearAnalyticRaySetup();

  // 3d volume raycasting: prepare clipped cube entry geometry (Vulkan uses
  // it directly; GL path also renders entry/exit textures inside prepareEntryExit()).
  ZMesh cube = ZMesh::createCube(glm::vec3(xCoordStart, yCoordStart, zCoordStart),
                                 glm::vec3(xCoordEnd, yCoordEnd, zCoordEnd),
                                 glm::vec3(xTexCoordStart, yTexCoordStart, zTexCoordStart),
                                 glm::vec3(xTexCoordEnd, yTexCoordEnd, zTexCoordEnd));
  cube.transformVerticesByMatrix(coordTransform);
  const bool flipped = glm::determinant(glm::mat3(coordTransform)) < 0.0f;

  std::vector<glm::vec3> planeNormals;
  std::vector<glm::vec3> planeOrigins;
  planeNormals.push_back(m_globalParameters.camera.get().viewVector());
  planeOrigins.push_back(m_globalParameters.camera.get().eye() +
                         m_globalParameters.camera.get().viewVector() *
                           (m_globalParameters.camera.get().nearDist() + 0.01f));
  if (m_globalParameters.globalXCut.lowerValue() != m_globalParameters.globalXCut.minimum()) {
    planeNormals.emplace_back(1., 0., 0.);
    planeOrigins.emplace_back(m_globalParameters.globalXCut.lowerValue(), 0, 0);
  }
  if (m_globalParameters.globalXCut.upperValue() != m_globalParameters.globalXCut.maximum()) {
    planeNormals.emplace_back(-1., 0., 0.);
    planeOrigins.emplace_back(m_globalParameters.globalXCut.upperValue(), 0, 0);
  }
  if (m_globalParameters.globalYCut.lowerValue() != m_globalParameters.globalYCut.minimum()) {
    planeNormals.emplace_back(0., 1., 0.);
    planeOrigins.emplace_back(0, m_globalParameters.globalYCut.lowerValue(), 0);
  }
  if (m_globalParameters.globalYCut.upperValue() != m_globalParameters.globalYCut.maximum()) {
    planeNormals.emplace_back(0., -1., 0.);
    planeOrigins.emplace_back(0, m_globalParameters.globalYCut.upperValue(), 0);
  }
  if (m_globalParameters.globalZCut.lowerValue() != m_globalParameters.globalZCut.minimum()) {
    planeNormals.emplace_back(0., 0., 1.);
    planeOrigins.emplace_back(0, 0, m_globalParameters.globalZCut.lowerValue());
  }
  if (m_globalParameters.globalZCut.upperValue() != m_globalParameters.globalZCut.maximum()) {
    planeNormals.emplace_back(0., 0., -1.);
    planeOrigins.emplace_back(0, 0, m_globalParameters.globalZCut.upperValue());
  }

  ZMesh clipped = ZMesh::clipClosedSurface(cube, planeNormals, planeOrigins);
  m_imgRaycasterRenderer.prepareEntryExit(clipped, flipped, eye, outputSize);
}

double Z3DImgFilter::renderImage(Z3DEye eye)
{
  CHECK(m_rendererBase.activeBackend() != RenderBackend::Vulkan)
    << "Z3DImgFilter::renderImage is OpenGL-only; Vulkan uses ZVulkanLinearScript in process().";

  Z3DRenderTarget& currentTarget = transparentTarget(eye);

  // VLOG(1) << m_progressiveRendering << " " << m_imgRaycasterRenderer.renderingStarted(eye);
  if (!(m_progressiveRendering && m_imgRaycasterRenderer.renderingStarted(eye))) {
    prepareRaycasterInputs(eye, currentTarget.size());
  }

  currentTarget.bind();
  currentTarget.clear();
  setViewport(currentTarget.size());

  m_transparentValid[eye] = false;
  auto targetGuard = folly::makeGuard([&currentTarget, this, eye]() {
    currentTarget.release();
    m_transparentValid[eye] = true;
  });

  double progress = 1.0;
  if (!m_progressiveRendering) {
    m_rendererBase.render(eye, m_imgRaycasterRenderer);
  } else {
    progress = m_imgRaycasterRenderer.renderProgressively(eye);
  }

  // Draw bound box with local overlay state
  renderBoundBox(eye, Z3DBoundedFilter::BoundBoxRenderStyle::OverlayAlphaDepth);
  CHECK_GL_ERROR

  // Entry/exit is only needed within a single frame for GL rendering when not
  // progressively accumulating. When progressive, keep it until completion.
  if (!m_progressiveRendering || progress >= 1.0) {
    m_imgRaycasterRenderer.releaseEntryExit();
  }

  return progress;
}

bool Z3DImgFilter::onlyBoundBox() const
{
  return !hasImage() && !m_boundBoxMode.isSelected("No Bound Box");
}

void Z3DImgFilter::renderOnlyBoundBox(Z3DEye eye)
{
  CHECK(m_rendererBase.activeBackend() != RenderBackend::Vulkan)
    << "Z3DImgFilter::renderOnlyBoundBox is OpenGL-only; Vulkan uses ZVulkanLinearScript in process().";

  Z3DRenderTarget& currentTarget = transparentTarget(eye);

  currentTarget.bind();
  currentTarget.clear();
  setViewport(currentTarget.size());

  m_transparentValid[eye] = false;
  auto targetGuard = folly::makeGuard([&currentTarget, this, eye]() {
    currentTarget.release();
    m_transparentValid[eye] = true;
  });

  // Draw bound box with local overlay state
  renderBoundBox(eye, Z3DBoundedFilter::BoundBoxRenderStyle::OverlayAlphaDepth);
  CHECK_GL_ERROR
}

void Z3DImgFilter::updateNotTransformedBoundBoxImpl()
{
  m_notTransformedBoundBox.setMinCorner(glm::dvec3(0.0));
  m_notTransformedBoundBox.setMaxCorner(glm::dvec3(m_3dImg->dimensions()));
}

void Z3DImgFilter::updateBlockIDTarget()
{
  //  if (m_3dImg && m_3dImg->isVolumeDownsampled()) {
  //    glm::uvec2 size = m_layerTarget.size();
  ////    uint32_t sizeScale =
  ////      std::min(std::min(Z3DImg::imageBlockSize().x, Z3DImg::imageBlockSize().y), Z3DImg::imageBlockSize().z) / 10;
  //    uint32_t sizeScale = 1;
  //    size.x = (size.x + sizeScale - 1) / sizeScale;
  //    size.y = (size.y + sizeScale - 1) / sizeScale;
  //    m_blockIDsRenderTarget.resize(size);
  //  }
}

// void Z3DImgFilter::invalidateAllFRVolumeSlices()
//{
//   m_FRVolumeSlicesValidState.clear();
//   m_FRVolumeSlicesValidState.resize(m_maxNumOfFullResolutionVolumeSlice, false);
// }

void Z3DImgFilter::volumeChanged() {}

void Z3DImgFilter::channelRangeChanged()
{
  m_channelRangeChanged = true;
}

bool Z3DImgFilter::depthPickAtScreenPoint(int x,
                                          int y,
                                          int width,
                                          int height,
                                          glm::ivec2& outPos2D,
                                          int& outTargetWidth,
                                          int& outTargetHeight,
                                          float& outDepth) const
{
  if (!m_imgRaycasterRenderer.hasVisibleRendering() || !m_3dImg) {
    return false;
  }

  const bool monoValid = m_transparentValid[MonoEye] && static_cast<bool>(m_transparentTargets[MonoEye]);
  const bool rightValid = m_transparentValid[RightEye] && static_cast<bool>(m_transparentTargets[RightEye]);
  if (!monoValid && !rightValid) {
    return false;
  }

  const Z3DScratchResourcePool::RenderTargetLease& lease =
    monoValid ? m_transparentTargets[MonoEye] : m_transparentTargets[RightEye];
  const glm::uvec2 targetSize =
    (lease.descriptor.size.x > 0u && lease.descriptor.size.y > 0u)
      ? lease.descriptor.size
      : glm::uvec2(static_cast<uint32_t>(std::max(1, width)), static_cast<uint32_t>(std::max(1, height)));
  outTargetWidth = static_cast<int>(targetSize.x);
  outTargetHeight = static_cast<int>(targetSize.y);

  outPos2D = glm::ivec2(x, outTargetHeight - y);
  outPos2D.x = std::clamp(outPos2D.x, 0, outTargetWidth - 1);
  outPos2D.y = std::clamp(outPos2D.y, 0, outTargetHeight - 1);

  if (lease.hasGLRenderTarget()) {
    outDepth = lease.glRenderTarget().depthAtPos(outPos2D);
    return true;
  }

  if (!lease.hasVulkanImage()) {
    return false;
  }

  ZVulkanTexture* depthTex = lease.depthAttachmentTexture();
  if (depthTex == nullptr) {
    return false;
  }

  float depth = 1.0f;
  try {
    depthTex->downloadSubImage(&depth,
                               sizeof(depth),
                               vk::Offset3D{outPos2D.x, outPos2D.y, 0},
                               vk::Extent3D{1u, 1u, 1u},
                               vk::ImageAspectFlagBits::eDepth);
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Vulkan depth download failed: " << e.what();
    return false;
  }

  outDepth = depth;
  return true;
}

bool Z3DImgFilter::unprojectDepthToImageCoordRounded(const glm::ivec2& pos2D,
                                                     double depth,
                                                     int targetWidth,
                                                     int targetHeight,
                                                     glm::vec3& outCoord)
{
  CHECK(m_3dImg);
  const glm::vec3 fpos3D = get3DPosition(pos2D, depth, targetWidth, targetHeight);
  outCoord = glm::round(glm::applyMatrix(inverseCoordTransform(), fpos3D));
  return glm::all(glm::greaterThanEqual(outCoord, glm::vec3(0.f))) &&
         glm::all(glm::lessThan(outCoord, glm::vec3(m_3dImg->dimensions())));
}

glm::vec3 Z3DImgFilter::getFirstHit3DPosition(int x, int y, int width, int height, bool& success)
{
  glm::vec3 res(-1);
  success = false;

  glm::ivec2 pos2D(0, 0);
  int targetWidth = 0;
  int targetHeight = 0;
  float depth = 1.0f;
  if (!depthPickAtScreenPoint(x, y, width, height, pos2D, targetWidth, targetHeight, depth)) {
    return res;
  }

  success = unprojectDepthToImageCoordRounded(pos2D, static_cast<double>(depth), targetWidth, targetHeight, res);
  return res;
}

glm::vec3 Z3DImgFilter::getMaxInten3DPositionUnderScreenPoint(int x, int y, int width, int height, bool& success)
{
  glm::vec3 res(-1);
  glm::vec3 des(-1);
  success = false;

  if (!m_3dImg) {
    return res;
  }

  const ZImgInfo info = m_3dImg->imgPack().imgInfo();
  if (m_seedTraceSourceChannel >= info.numChannels) {
    return glm::vec3(-1);
  }

  glm::ivec2 pos2D(0, 0);
  int targetWidth = 0;
  int targetHeight = 0;
  float depth = 1.0f;
  if (!depthPickAtScreenPoint(x, y, width, height, pos2D, targetWidth, targetHeight, depth)) {
    return res;
  }

  success = unprojectDepthToImageCoordRounded(pos2D, static_cast<double>(depth), targetWidth, targetHeight, res);

  if (success) {
    (void)unprojectDepthToImageCoordRounded(pos2D, 1.0, targetWidth, targetHeight, des);
    if (glm::length(des - res) <= 1.f) { // res is last pixel along current ray direction
      return res;
    }
  }

  // Find maximum intensity voxel start from res along des direction.
  if (success) {
    double maxInten = m_3dImg->imgPack().value(res.x, res.y, res.z, m_seedTraceSourceChannel, 0);
    glm::vec3 p = res;
    glm::vec3 d = des - res;
    float N = std::max(std::max(std::abs(d.x), std::abs(d.y)), std::abs(d.z));
    glm::vec3 stepSize = d / N;
    while (true) {
      p = p + stepSize;
      glm::vec3 roundP = glm::round(p);
      if (roundP.x < 0 || roundP.x >= m_3dImg->imgPack().imgInfo().width || roundP.y < 0 ||
          roundP.y >= m_3dImg->imgPack().imgInfo().height || roundP.z < 0 ||
          roundP.z >= m_3dImg->imgPack().imgInfo().depth) {
        break;
      }
      double inten = m_3dImg->imgPack().value(roundP.x, roundP.y, roundP.z, m_seedTraceSourceChannel, 0);
      if (inten > maxInten) {
        maxInten = inten;
        res = roundP;
      }
    }
  }
  return res;
}

glm::vec3 Z3DImgFilter::get3DPosition(glm::ivec2 pos2D, int width, int height, Z3DRenderTarget& target)
{
  glm::mat4 projection = m_globalParameters.camera.get().projectionMatrix(MonoEye);
  glm::mat4 modelview = m_globalParameters.camera.get().viewMatrix(MonoEye);

  glm::ivec4 viewport;
  viewport[0] = 0;
  viewport[1] = 0;
  viewport[2] = width;
  viewport[3] = height;

  GLfloat WindowPosZ = target.depthAtPos(pos2D);
  const glm::vec3 win(pos2D.x, pos2D.y, WindowPosZ);
  glm::vec3 pos;
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    pos = glm::unProjectZO(win, modelview, projection, viewport);
  } else {
    pos = glm::unProjectNO(win, modelview, projection, viewport);
  }

  return pos;
}

glm::vec3 Z3DImgFilter::get3DPosition(glm::ivec2 pos2D, double depth, int width, int height)
{
  glm::mat4 projection = m_globalParameters.camera.get().projectionMatrix(MonoEye);
  glm::mat4 modelview = m_globalParameters.camera.get().viewMatrix(MonoEye);

  glm::ivec4 viewport;
  viewport[0] = 0;
  viewport[1] = 0;
  viewport[2] = width;
  viewport[3] = height;

  const glm::vec3 win(pos2D.x, pos2D.y, depth);
  glm::vec3 pos;
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    pos = glm::unProjectZO(win, modelview, projection, viewport);
  } else {
    pos = glm::unProjectNO(win, modelview, projection, viewport);
  }

  return pos;
}

Z3DRenderTarget& Z3DImgFilter::transparentTarget(Z3DEye eye)
{
  return ensureRenderTarget(m_transparentTargets[eye]);
}

Z3DRenderTarget& Z3DImgFilter::opaqueTarget(Z3DEye eye)
{
  return ensureRenderTarget(m_opaqueTargets[eye]);
}

Z3DRenderTarget& Z3DImgFilter::ensureRenderTarget(Z3DScratchResourcePool::RenderTargetLease& lease)
{
  if (!lease.renderTarget || lease.renderTarget->size() != m_outputSize) {
    lease.release();
    CHECK_GT(m_outputSize.x, 0u);
    CHECK_GT(m_outputSize.y, 0u);
    m_rendererBase.acquirePersistentTempRenderTarget2D(lease, m_outputSize);
  }
  return *lease.renderTarget;
}

void Z3DImgFilter::releaseAllRenderTargets()
{
  for (auto& lease : m_transparentTargets) {
    lease.release();
  }
  for (auto& lease : m_opaqueTargets) {
    lease.release();
  }
}

void Z3DImgFilter::markTargetsInvalid()
{
  m_transparentValid.fill(false);
  m_opaqueValid.fill(false);
}

} // namespace nim
