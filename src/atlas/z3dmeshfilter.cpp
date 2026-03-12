#include "z3dmeshfilter.h"

#include "z3dmeshrenderer.h"
#include "z3dtextureglowrenderer.h"
#include "zlog.h"
#include "zmesh.h"
#include "zrandom.h"
#include "zregionannotation.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent>

#include <algorithm>
#include <ranges>

namespace nim {

namespace {

// Runtime Neuroglancer mesh LOD uses two screen-space targets:
// - a looser interaction target to keep camera motion responsive
// - Neuroglancer's default per-pixel target once motion settles
//
// Neuroglancer's `renderScaleTarget` is not a large absolute number; the
// default is `1`, meaning the chosen mesh resolution should be no worse than
// the current pixel footprint. Our first Atlas integration accidentally used
// the large numbers from unit-test examples (`1000`/`4000`) as runtime policy
// values, which made the coarsest loaded rows effectively "good enough" for
// almost every view and prevented visible refinement.
constexpr float kRuntimeNgInteractionDetailCutoff = 4.0F;
constexpr float kRuntimeNgIdleDetailCutoff = 1.0F;
constexpr int kRuntimeNgIdleDelayMs = 180;

struct RuntimeNeuroglancerOpenResult
{
  uint64_t epoch = 0;
  QString error;
  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source;
  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodManifest> manifest;
  std::vector<uint32_t> baseRows;
};

struct RuntimeNeuroglancerChunkBatchResult
{
  uint64_t epoch = 0;
  QString error;
  std::vector<uint32_t> loadedRows;
  std::vector<std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh>> loadedChunks;
  std::vector<uint32_t> failedRows;
};

[[nodiscard]] bool sameOptionalArray(const std::optional<std::array<double, 3>>& lhs,
                                     const std::optional<std::array<double, 3>>& rhs)
{
  return lhs == rhs;
}

[[nodiscard]] bool sameOptionalArray(const std::optional<std::array<int64_t, 3>>& lhs,
                                     const std::optional<std::array<int64_t, 3>>& rhs)
{
  return lhs == rhs;
}

[[nodiscard]] std::vector<uint32_t>
collectRuntimeNeuroglancerBaseRows(const ZNeuroglancerPrecomputedMeshSource::MultiLodManifest& manifest)
{
  std::vector<uint32_t> baseRows;
  const uint32_t coarsestLod = manifest.coarsestStoredLod().value_or(0U);
  for (uint32_t row = 0; row < manifest.octreeNodes.size(); ++row) {
    if (manifest.rowLods[row] != coarsestLod) {
      continue;
    }
    if (manifest.octreeNodes[row].empty()) {
      continue;
    }
    if (manifest.offsets[row + 1U] <= manifest.offsets[row]) {
      continue;
    }
    baseRows.push_back(row);
  }
  return baseRows;
}

[[nodiscard]] RuntimeNeuroglancerOpenResult openRuntimeNeuroglancerSource(uint64_t epoch,
                                                                          const ZNeuroglancerMeshExternalSourceKey& key)
{
  RuntimeNeuroglancerOpenResult out;
  out.epoch = epoch;
  try {
    CHECK(key.baseResolutionNm);
    CHECK(key.baseVoxelOffset);
    const auto source = ZNeuroglancerPrecomputedMeshSource::open(QUrl(key.meshSourceDirUrl),
                                                                 *key.baseResolutionNm,
                                                                 *key.baseVoxelOffset,
                                                                 std::chrono::milliseconds(30000));
    CHECK(source);
    if (!source->supportsRuntimeLod()) {
      out.error = QStringLiteral("dataset does not support multiscale mesh LOD");
      return out;
    }

    out.source = source;
    out.manifest = source->loadManifestBlocking(key.segmentId);
    CHECK(out.manifest);
    out.baseRows = collectRuntimeNeuroglancerBaseRows(*out.manifest);
  }
  catch (const std::exception& e) {
    out.error = QString::fromUtf8(e.what());
  }
  return out;
}

} // namespace

Z3DMeshFilter::Z3DMeshFilter(Z3DGlobalParameters& globalParas, const RegionNode* regionNode, QObject* parent)
  : Z3DGeometryFilter(globalParas, parent)
  , m_triangleListRenderer(m_rendererBase)
  , m_wireframeMode("Wireframe Option")
  , m_wireframeColor("Wireframe Color", glm::vec4(1), glm::vec4(0), glm::vec4(1))
  , m_colorMode("Color Mode")
  , m_singleColorForAllMesh("Mesh Color",
                            glm::vec4(ZRandom::instance().randReal<float>(),
                                      ZRandom::instance().randReal<float>(),
                                      ZRandom::instance().randReal<float>(),
                                      1.f))
  , m_glow("Glow", false)
  , m_glowMode("Glow Mode")
  , m_glowBlurRadius("Glow Blur Radius", 10, 2, 1000)
  , m_glowBlurScale("Glow Blur Scale", 1.f, 1.f, 5.f)
  , m_glowBlurStrength("Glow Blur Strength", .5f, 0.f, 1.f)
  , m_selectMeshEvent("Select Mesh", false)
  , m_pressedMesh(nullptr)
  , m_selectedMeshes(nullptr)
  , m_dataIsInvalid(false)
  , m_regionNode(regionNode)
{
  m_singleColorForAllMesh.setStyle("COLOR");
  connect(&m_singleColorForAllMesh, &ZVec4Parameter::valueChanged, this, &Z3DMeshFilter::prepareColor);

  // Color Mode
  m_colorMode.addOptions("Mesh Color", "Single Color");
  m_colorMode.select("Single Color");
  m_colorMode.setDescription(QStringLiteral(
    "Controls how mesh surface colors are chosen:\n"
    "- 'Single Color' (recommended when you want to control color) applies the 'Mesh Color' parameter below as a solid color for the whole mesh.\n"
    "- 'Mesh Color' reads color attributes embedded in the mesh file (per-vertex/face). In this mode you cannot override the color with parameters;\n"
    "  if the mesh lacks embedded colors, the fill color falls back to the contained/default color (often black). Choose 'Single Color' to force a specific color."));

  connect(&m_colorMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DMeshFilter::prepareColor);
  connect(&m_colorMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DMeshFilter::adjustWidgets);

  m_wireframeMode.addOptionsWithData(
    std::make_pair("No Wireframe", static_cast<int>(Z3DMeshRenderer::WireframeMode::NoWireframe)),
    std::make_pair("With Wireframe", static_cast<int>(Z3DMeshRenderer::WireframeMode::WithWireframe)),
    std::make_pair("Only Wireframe", static_cast<int>(Z3DMeshRenderer::WireframeMode::OnlyWireframe)));
  m_wireframeMode.select("No Wireframe");
  m_wireframeColor.setStyle("COLOR");
  updateWireframeMode();
  updateWireframeColor();
  connect(&m_wireframeMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DMeshFilter::updateWireframeMode);
  connect(&m_wireframeColor, &ZVec4Parameter::valueChanged, this, &Z3DMeshFilter::updateWireframeColor);

  addParameter(m_colorMode);

  addParameter(m_singleColorForAllMesh);
  m_singleColorForAllMesh.setDescription(
    QStringLiteral("Solid RGBA color used when 'Color Mode' is set to 'Single Color'."));

  addParameter(m_wireframeMode);
  addParameter(m_wireframeColor);
  m_wireframeMode.setDescription(QStringLiteral("Render meshes with or without a wireframe overlay."));
  m_wireframeColor.setDescription(QStringLiteral("Wireframe line color (applies when wireframe is visible)."));

  connect(&m_glow, &ZBoolParameter::valueChanged, this, &Z3DMeshFilter::adjustWidgets);
  addParameter(m_glow);
  // Initialize glow parameter defaults to match previous behavior
  m_glowMode.clearOptions();
  m_glowMode.addOptionsWithData(
    std::make_pair(enumToQString(GlowMode::Additive), static_cast<int>(GlowMode::Additive)),
    std::make_pair(enumToQString(GlowMode::Screen), static_cast<int>(GlowMode::Screen)),
    std::make_pair(enumToQString(GlowMode::Softlight), static_cast<int>(GlowMode::Softlight)),
    std::make_pair(enumToQString(GlowMode::Glowmap), static_cast<int>(GlowMode::Glowmap)));
  m_glowMode.select(enumToQString(GlowMode::Screen));
  m_glowBlurScale.setSingleStep(0.5f);
  addParameter(m_glowMode);
  addParameter(m_glowBlurRadius);
  addParameter(m_glowBlurScale);
  addParameter(m_glowBlurStrength);
  m_glow.setDescription(QStringLiteral("Enable a post-processing glow around bright surfaces."));
  m_glowMode.setDescription(QStringLiteral("Blend mode for the glow pass."));
  m_glowBlurRadius.setDescription(QStringLiteral("Radius of the glow blur kernel (pixels)."));
  m_glowBlurScale.setDescription(QStringLiteral("Scale factor applied to the glow blur radius."));
  m_glowBlurStrength.setDescription(QStringLiteral("Opacity applied when compositing the glow."));

  m_selectMeshEvent.listenTo("select mesh", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
  m_selectMeshEvent.listenTo("select mesh", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonRelease);
  m_selectMeshEvent.listenTo("select mesh", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonDblClick);
  m_selectMeshEvent.listenTo("select mesh", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonDblClick);
  m_selectMeshEvent.listenTo("append select mesh", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonPress);
  m_selectMeshEvent.listenTo("append select mesh", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonRelease);
  connect(&m_selectMeshEvent, &ZEventListenerParameter::mouseEventTriggered, this, &Z3DMeshFilter::selectMesh);
  addEventListener(m_selectMeshEvent);

  m_runtimeNgIdleTimer.setSingleShot(true);
  connect(&m_runtimeNgIdleTimer, &QTimer::timeout, this, &Z3DMeshFilter::onRuntimeNeuroglancerIdleTimeout);
  connect(&m_globalParameters.camera,
          &Z3DCameraParameter::valueChanged,
          this,
          &Z3DMeshFilter::onRuntimeNeuroglancerCameraChanged);
  connect(this,
          &Z3DBoundedFilter::rendererCoordTransformChanged,
          this,
          &Z3DMeshFilter::markRuntimeNeuroglancerLodDirty);

  adjustWidgets();
}

QString Z3DMeshFilter::regionName() const
{
  return m_regionNode ? m_regionNode->name : QString();
}

double Z3DMeshFilter::process(Z3DEye)
{
  syncRendererState();
  applyRuntimeNeuroglancerSelection();

  if (m_dataIsInvalid) {
    prepareData();
  }

  return 1.;
}

void Z3DMeshFilter::setProgressiveRenderingMode(bool v)
{
  m_runtimeNgProgressiveRendering = v;
}

void Z3DMeshFilter::beginExportMeshLod(const glm::uvec2& fullViewport)
{
  if (!m_runtimeNgSourceKey || fullViewport.x == 0U || fullViewport.y == 0U) {
    return;
  }

  m_runtimeNgIdleTimer.stop();
  m_runtimeNgInteractionActive = false;

  if (!m_runtimeNgSource || !m_runtimeNgManifest) {
    const RuntimeNeuroglancerOpenResult openResult =
      openRuntimeNeuroglancerSource(m_runtimeNgEpoch, *m_runtimeNgSourceKey);
    if (!openResult.error.isEmpty()) {
      LOG(WARNING) << fmt::format("Mesh export LOD preload skipped: {}", openResult.error);
      return;
    }
    m_runtimeNgSource = openResult.source;
    m_runtimeNgManifest = openResult.manifest;
    m_runtimeNgBaseRows = openResult.baseRows;
    m_runtimeNgBaseReady = m_runtimeNgBaseRows.empty();
  }

  CHECK(m_runtimeNgSource);
  CHECK(m_runtimeNgManifest);

  m_runtimeNgFailedRows.clear();

  const glm::mat4 modelViewProjection =
    m_globalParameters.camera.get().projectionViewMatrix(MonoEye) * coordTransform();
  const auto clippingPlanes = ZNeuroglancerPrecomputedMeshSource::getFrustumPlanes(modelViewProjection);
  const auto desiredRows = ZNeuroglancerPrecomputedMeshSource::desiredChunksForView(*m_runtimeNgManifest,
                                                                                    modelViewProjection,
                                                                                    clippingPlanes,
                                                                                    kRuntimeNgIdleDetailCutoff,
                                                                                    fullViewport.x,
                                                                                    fullViewport.y);

  std::vector<uint32_t> rowsToLoad = m_runtimeNgBaseRows;
  rowsToLoad.reserve(rowsToLoad.size() + desiredRows.size());
  for (const auto& desired : desiredRows) {
    if (desired.empty) {
      continue;
    }
    rowsToLoad.push_back(desired.row);
  }
  std::ranges::sort(rowsToLoad);
  rowsToLoad.erase(std::unique(rowsToLoad.begin(), rowsToLoad.end()), rowsToLoad.end());

  QString firstError;
  for (const uint32_t row : rowsToLoad) {
    if (m_runtimeNgLoadedRows.contains(row)) {
      continue;
    }
    try {
      auto chunkMesh = m_runtimeNgSource->loadChunkMeshBlocking(m_runtimeNgSourceKey->segmentId, row);
      CHECK(chunkMesh);
      m_runtimeNgLoadedRows[row] = std::move(chunkMesh);
    }
    catch (const std::exception& e) {
      m_runtimeNgFailedRows.insert(row);
      if (firstError.isEmpty()) {
        firstError = QString::fromUtf8(e.what());
      }
    }
  }

  if (!firstError.isEmpty()) {
    LOG(WARNING) << fmt::format("Mesh export LOD preload had failures: {}", firstError);
  }

  m_runtimeNgBaseReady = std::ranges::all_of(m_runtimeNgBaseRows, [this](uint32_t row) {
    return m_runtimeNgLoadedRows.contains(row);
  });

  const auto drawChunks = ZNeuroglancerPrecomputedMeshSource::chunksToDrawForView(
    *m_runtimeNgManifest,
    modelViewProjection,
    clippingPlanes,
    kRuntimeNgIdleDetailCutoff,
    fullViewport.x,
    fullViewport.y,
    [this](uint32_t /*lod*/, uint32_t row, float /*renderScale*/) {
      return m_runtimeNgLoadedRows.contains(row);
    });

  std::vector<ZMesh*> frozenVisibleMeshes;
  for (const auto& drawChunk : drawChunks) {
    auto it = m_runtimeNgLoadedRows.find(drawChunk.row);
    if (it == m_runtimeNgLoadedRows.end()) {
      continue;
    }
    const auto& chunkMesh = it->second;
    CHECK(chunkMesh);
    CHECK(drawChunk.subChunkBegin <= drawChunk.subChunkEnd);
    CHECK(drawChunk.subChunkEnd <= chunkMesh->subMeshes.size());
    for (uint32_t i = drawChunk.subChunkBegin; i < drawChunk.subChunkEnd; ++i) {
      const auto& subMesh = chunkMesh->subMeshes[i];
      if (subMesh && !subMesh->empty()) {
        frozenVisibleMeshes.push_back(subMesh.get());
      }
    }
  }

  m_runtimeNgFrozenVisibleMeshes = std::move(frozenVisibleMeshes);
  m_runtimeNgExportActive = true;
  getVisibleData();
  m_dataIsInvalid = true;
  invalidateResult();
}

void Z3DMeshFilter::endExportMeshLod()
{
  if (!m_runtimeNgExportActive) {
    return;
  }

  m_runtimeNgExportActive = false;
  m_runtimeNgFrozenVisibleMeshes.clear();
  m_runtimeNgSelectionDirty = true;
  getVisibleData();
  m_dataIsInvalid = true;
  invalidateResult();
}

void Z3DMeshFilter::setData(std::vector<ZMesh*>* meshList)
{
  m_origMeshList.clear();
  if (meshList) {
    m_origMeshList = *meshList;
    LOG(INFO) << className() << " read " << m_origMeshList.size() << " meshes.";
  }
  getVisibleData();
  m_dataIsInvalid = true;
  invalidateResult();

  updateBoundBox();
  initializeRotationCenterIfDefault();
}

void Z3DMeshFilter::setExternalSourceJson(json::value sourceJson)
{
  const auto keyOpt = parseNeuroglancerMeshExternalSourceKey(sourceJson);
  if (!keyOpt || keyOpt->meshSourceDirUrl.isEmpty() || !keyOpt->baseResolutionNm || !keyOpt->baseVoxelOffset) {
    if (!m_runtimeNgSourceKey && m_externalSourceJson == sourceJson) {
      return;
    }
    m_externalSourceJson = std::move(sourceJson);
    resetRuntimeNeuroglancerLodState();
    return;
  }

  if (isSameRuntimeNeuroglancerSource(*keyOpt) && m_externalSourceJson == sourceJson) {
    return;
  }

  m_externalSourceJson = std::move(sourceJson);
  resetRuntimeNeuroglancerLodState();
  m_runtimeNgSourceKey = *keyOpt;
  startRuntimeNeuroglancerOpen();
}

bool Z3DMeshFilter::isReady(Z3DEye eye) const
{
  return Z3DGeometryFilter::isReady(eye) && m_visible.get() && !m_origMeshList.empty();
}

std::shared_ptr<ZWidgetsGroup> Z3DMeshFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Mesh", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_stayOnTop, 1);
    m_widgetsGroup->addChild(m_colorMode, 1);
    m_widgetsGroup->addChild(m_singleColorForAllMesh, 1);
    m_widgetsGroup->addChild(m_wireframeMode, 1);
    m_widgetsGroup->addChild(m_wireframeColor, 1);

    m_widgetsGroup->addChild(m_rendererParameters.coordTransform, 2);
    m_widgetsGroup->addChild(m_rendererParameters.opacity, 5);
    m_widgetsGroup->addChild(m_rendererParameters.materialAmbient, 7);
    m_widgetsGroup->addChild(m_rendererParameters.materialSpecular, 7);
    m_widgetsGroup->addChild(m_rendererParameters.materialShininess, 7);

    m_widgetsGroup->addChild(m_glow, 5);
    m_widgetsGroup->addChild(m_glowMode, 5);
    m_widgetsGroup->addChild(m_glowBlurRadius, 5);
    m_widgetsGroup->addChild(m_glowBlurScale, 5);
    m_widgetsGroup->addChild(m_glowBlurStrength, 5);

    m_widgetsGroup->addChild(m_xCut, 5);
    m_widgetsGroup->addChild(m_yCut, 5);
    m_widgetsGroup->addChild(m_zCut, 5);
    m_widgetsGroup->addChild(m_boundBoxMode, 5);
    m_widgetsGroup->addChild(m_boundBoxLineWidth, 5);
    m_widgetsGroup->addChild(m_boundBoxLineColor, 5);
    m_widgetsGroup->addChild(m_selectionLineWidth, 7);
    m_widgetsGroup->addChild(m_selectionLineColor, 7);
    m_widgetsGroup->addChild(m_manipulatorSize, 7);
    m_widgetsGroup->setBasicAdvancedCutoff(5);
  }
  return m_widgetsGroup;
}

std::shared_ptr<ZWidgetsGroup> Z3DMeshFilter::widgetsGroupForAnnotationFilter()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Mesh", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_singleColorForAllMesh, 1);
    m_widgetsGroup->addChild(m_wireframeMode, 1);
    m_widgetsGroup->addChild(m_wireframeColor, 1);

    m_widgetsGroup->addChild(m_rendererParameters.opacity, 5);
    m_widgetsGroup->addChild(m_rendererParameters.materialAmbient, 5);
    m_widgetsGroup->addChild(m_rendererParameters.materialSpecular, 5);
    m_widgetsGroup->addChild(m_rendererParameters.materialShininess, 5);
    m_widgetsGroup->addChild(m_xCut, 5);
    m_widgetsGroup->addChild(m_yCut, 5);
    m_widgetsGroup->addChild(m_zCut, 5);
    m_widgetsGroup->addChild(m_boundBoxMode, 5);
    m_widgetsGroup->addChild(m_boundBoxLineWidth, 5);
    m_widgetsGroup->addChild(m_boundBoxLineColor, 5);
    m_widgetsGroup->setBasicAdvancedCutoff(5);
  }
  return m_widgetsGroup;
}

void Z3DMeshFilter::renderOpaque(Z3DEye eye)
{
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    m_rendererBase.renderVulkan(eye, m_triangleListRenderer);
  } else {
    m_rendererBase.render(eye, m_triangleListRenderer);
  }
  renderBoundBox(eye);
}

void Z3DMeshFilter::renderTransparent(Z3DEye eye)
{
  if (m_glow.get()) {
    // Compositor owns glow composition; only render bound box here
    renderBoundBox(eye);
  } else {
    if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
      m_rendererBase.renderVulkan(eye, m_triangleListRenderer);
    } else {
      m_rendererBase.render(eye, m_triangleListRenderer);
    }
    renderBoundBox(eye);
  }
}

void Z3DMeshFilter::renderPicking(Z3DEye eye)
{
  if (!m_pickingObjectsRegistered) {
    registerPickingObjects();
  }
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    m_rendererBase.renderPickingVulkan(eye, m_triangleListRenderer);
  } else {
    m_rendererBase.renderPicking(eye, m_triangleListRenderer);
  }
}

void Z3DMeshFilter::setViewport(glm::uvec2 viewport)
{
  Z3DBoundedFilter::setViewport(viewport);
  markRuntimeNeuroglancerLodDirty();
}

void Z3DMeshFilter::setViewport(glm::uvec4 viewport)
{
  Z3DBoundedFilter::setViewport(viewport);
  markRuntimeNeuroglancerLodDirty();
}

void Z3DMeshFilter::prepareData()
{
  if (!m_dataIsInvalid) {
    return;
  }

  deregisterPickingObjects();

  m_triangleListRenderer.setData(&m_meshList);
  prepareColor();
  adjustWidgets();
  m_dataIsInvalid = false;
}

void Z3DMeshFilter::registerPickingObjects()
{
  if (!m_pickingObjectsRegistered) {
    for (auto mesh : m_meshList) {
      pickingManager().registerObject(mesh);
    }
    m_registeredMeshList = m_meshList;
    m_meshPickingColors.clear();
    for (auto mesh : m_meshList) {
      glm::col4 pickingColor = pickingManager().colorOfObject(mesh);
      glm::vec4 fPickingColor(pickingColor[0] / 255.f,
                              pickingColor[1] / 255.f,
                              pickingColor[2] / 255.f,
                              pickingColor[3] / 255.f);
      m_meshPickingColors.push_back(fPickingColor);
    }
    m_triangleListRenderer.setDataPickingColors(&m_meshPickingColors);
  }

  m_pickingObjectsRegistered = true;
}

void Z3DMeshFilter::deregisterPickingObjects()
{
  if (m_pickingObjectsRegistered) {
    for (auto mesh : m_registeredMeshList) {
      pickingManager().deregisterObject(mesh);
    }
    m_registeredMeshList.clear();
  }

  m_pickingObjectsRegistered = false;
}

ZBBox<glm::dvec3> Z3DMeshFilter::meshBound(ZMesh* p)
{
  auto it = m_meshBoundboxMapper.find(p);
  if (it != m_meshBoundboxMapper.end()) {
    return it->second;
  }

  ZBBox<glm::dvec3> result = p->boundBox(coordTransform());
  m_meshBoundboxMapper[p] = result;
  return result;
}

void Z3DMeshFilter::updateNotTransformedBoundBoxImpl()
{
  m_notTransformedBoundBox.reset();
  for (auto& mesh : m_origMeshList) {
    m_notTransformedBoundBox.expand(mesh->boundBox());
  }
}

void Z3DMeshFilter::prepareColor()
{
  m_meshColors.clear();

  if (m_colorMode.isSelected("Single Color")) {
    for (size_t i = 0; i < m_meshList.size(); ++i) {
      m_meshColors.push_back(m_singleColorForAllMesh.get());
    }
    m_triangleListRenderer.setDataColors(&m_meshColors);
    m_triangleListRenderer.setColorSource(MeshColorSource::CustomColor);
  } else if (m_colorMode.isSelected("Mesh Color")) {
    m_triangleListRenderer.setColorSource(MeshColorSource::MeshColor);
  }
}

void Z3DMeshFilter::updateWireframeMode()
{
  m_triangleListRenderer.setWireframeMode(
    static_cast<Z3DMeshRenderer::WireframeMode>(m_wireframeMode.associatedData()));
  adjustWidgets();
}

void Z3DMeshFilter::updateWireframeColor()
{
  m_triangleListRenderer.setWireframeColor(m_wireframeColor.get());
}

void Z3DMeshFilter::adjustWidgets()
{
  m_singleColorForAllMesh.setVisible(m_colorMode.isSelected("Single Color"));

  m_glowMode.setVisible(m_glow.get());
  m_glowBlurRadius.setVisible(m_glow.get());
  m_glowBlurScale.setVisible(m_glow.get());
  m_glowBlurStrength.setVisible(m_glow.get());
  m_wireframeColor.setVisible(!m_wireframeMode.isSelected("No Wireframe"));
}

void Z3DMeshFilter::selectMesh(QMouseEvent* e, int /*w*/, int /*h*/)
{
  if (m_meshList.empty()) {
    return;
  }

  e->ignore();
  if (e->type() == QEvent::MouseButtonDblClick) {
    const void* obj = pickingManager().objectAtWidgetPos(glm::ivec2(e->position().x(), e->position().y()));
    bool appending = (e->modifiers() == Qt::ControlModifier);
    if (!obj && !appending && m_isSelected) {
      Q_EMIT objDeselected();
      return;
    }
    bool hit = contains(m_meshList, static_cast<const ZMesh*>(obj));
    if (hit) {
      Q_EMIT objSelected(appending);
      e->accept();
    }
    return;
  }

  e->ignore();
  // Mouse button pressend
  // can not accept the event in button press, because we don't know if it is a selection or interaction
  if (e->type() == QEvent::MouseButtonPress) {
    m_startCoord.x = e->position().x();
    m_startCoord.y = e->position().y();
    const void* obj = pickingManager().objectAtWidgetPos(glm::ivec2(e->position().x(), e->position().y()));
    if (!obj) {
      return;
    }

    // Check if any point was selected...
    for (auto m : m_meshList) {
      if (m == obj) {
        m_pressedMesh = m;
        break;
      }
    }
    return;
  }

  if (e->type() == QEvent::MouseButtonRelease) {
    if (std::abs(e->position().x() - m_startCoord.x) < 2 && std::abs(m_startCoord.y - e->position().y()) < 2) {
      if (e->modifiers() == Qt::ControlModifier) {
        Q_EMIT meshSelected(m_pressedMesh, true);
      } else {
        Q_EMIT meshSelected(m_pressedMesh, false);
      }
      if (m_pressedMesh) {
        e->accept();
      }
    }
    m_pressedMesh = nullptr;
  }
}

void Z3DMeshFilter::onApplyTransform()
{
  VLOG(1) << m_rendererParameters.coordTransform.get();
}

void Z3DMeshFilter::updateMeshVisibleState()
{
  getVisibleData();
  m_dataIsInvalid = true;
  invalidateResult();
}

void Z3DMeshFilter::resetRuntimeNeuroglancerLodState()
{
  ++m_runtimeNgEpoch;
  m_runtimeNgIdleTimer.stop();
  m_runtimeNgSourceKey.reset();
  m_runtimeNgSource.reset();
  m_runtimeNgManifest.reset();
  m_runtimeNgBaseRows.clear();
  m_runtimeNgLoadedRows.clear();
  m_runtimeNgRowsInFlight.clear();
  m_runtimeNgFailedRows.clear();
  m_runtimeNgVisibleMeshes.clear();
  m_runtimeNgFrozenVisibleMeshes.clear();
  m_runtimeNgBaseReady = false;
  m_runtimeNgInteractionActive = false;
  m_runtimeNgSelectionDirty = false;
  m_runtimeNgExportActive = false;
  getVisibleData();
  m_dataIsInvalid = true;
  invalidateResult();
}

void Z3DMeshFilter::markRuntimeNeuroglancerLodDirty()
{
  if (!m_runtimeNgSourceKey || m_runtimeNgExportActive) {
    return;
  }
  m_runtimeNgFailedRows.clear();
  m_runtimeNgSelectionDirty = true;
  invalidateResult();
}

void Z3DMeshFilter::onRuntimeNeuroglancerCameraChanged()
{
  if (!m_runtimeNgSourceKey || m_runtimeNgExportActive) {
    return;
  }
  m_runtimeNgInteractionActive = true;
  m_runtimeNgIdleTimer.start(kRuntimeNgIdleDelayMs);
  markRuntimeNeuroglancerLodDirty();
}

void Z3DMeshFilter::onRuntimeNeuroglancerIdleTimeout()
{
  if (!m_runtimeNgInteractionActive || m_runtimeNgExportActive) {
    return;
  }
  m_runtimeNgInteractionActive = false;
  markRuntimeNeuroglancerLodDirty();
}

void Z3DMeshFilter::startRuntimeNeuroglancerOpen()
{
  CHECK(m_runtimeNgSourceKey);

  const uint64_t epoch = m_runtimeNgEpoch;
  const ZNeuroglancerMeshExternalSourceKey key = *m_runtimeNgSourceKey;

  auto* watcher = new QFutureWatcher<RuntimeNeuroglancerOpenResult>(this);
  connect(watcher, &QFutureWatcher<RuntimeNeuroglancerOpenResult>::finished, this, [this, watcher, epoch]() {
    const RuntimeNeuroglancerOpenResult result = watcher->result();
    watcher->deleteLater();
    if (result.epoch != epoch || epoch != m_runtimeNgEpoch) {
      return;
    }
    if (!result.error.isEmpty()) {
      VLOG(1) << fmt::format("Runtime Neuroglancer mesh LOD disabled: {}", result.error);
      return;
    }

    m_runtimeNgSource = result.source;
    m_runtimeNgManifest = result.manifest;
    m_runtimeNgBaseRows = result.baseRows;
    m_runtimeNgBaseReady = m_runtimeNgBaseRows.empty();
    m_runtimeNgSelectionDirty = true;
    if (!m_runtimeNgBaseRows.empty()) {
      requestRuntimeNeuroglancerRows(m_runtimeNgBaseRows);
    } else {
      invalidateResult();
    }
  });

  watcher->setFuture(QtConcurrent::run([epoch, key]() -> RuntimeNeuroglancerOpenResult {
    return openRuntimeNeuroglancerSource(epoch, key);
  }));
}

void Z3DMeshFilter::requestRuntimeNeuroglancerRows(const std::vector<uint32_t>& rows)
{
  if (!m_runtimeNgSourceKey || !m_runtimeNgSource || rows.empty()) {
    return;
  }

  std::vector<uint32_t> rowsToLoad;
  rowsToLoad.reserve(rows.size());
  for (const uint32_t row : rows) {
    if (m_runtimeNgLoadedRows.contains(row) || m_runtimeNgRowsInFlight.contains(row) ||
        m_runtimeNgFailedRows.contains(row)) {
      continue;
    }
    m_runtimeNgRowsInFlight.insert(row);
    rowsToLoad.push_back(row);
  }
  if (rowsToLoad.empty()) {
    return;
  }

  const uint64_t epoch = m_runtimeNgEpoch;
  const uint64_t segmentId = m_runtimeNgSourceKey->segmentId;
  const auto source = m_runtimeNgSource;

  auto* watcher = new QFutureWatcher<RuntimeNeuroglancerChunkBatchResult>(this);
  connect(watcher, &QFutureWatcher<RuntimeNeuroglancerChunkBatchResult>::finished, this, [this, watcher, epoch]() {
    const RuntimeNeuroglancerChunkBatchResult result = watcher->result();
    watcher->deleteLater();

    if (result.epoch != epoch || epoch != m_runtimeNgEpoch) {
      return;
    }

    for (const uint32_t row : result.loadedRows) {
      m_runtimeNgRowsInFlight.erase(row);
    }
    for (const uint32_t row : result.failedRows) {
      m_runtimeNgRowsInFlight.erase(row);
    }

    for (size_t i = 0; i < result.loadedRows.size(); ++i) {
      m_runtimeNgLoadedRows[result.loadedRows[i]] = result.loadedChunks[i];
    }
    for (const uint32_t row : result.failedRows) {
      m_runtimeNgFailedRows.insert(row);
    }

    if (!result.error.isEmpty()) {
      VLOG(1) << fmt::format("Runtime Neuroglancer mesh LOD chunk load had failures: {}", result.error);
    }

    if (!m_runtimeNgBaseReady) {
      m_runtimeNgBaseReady = std::ranges::all_of(m_runtimeNgBaseRows, [this](uint32_t row) {
        return m_runtimeNgLoadedRows.contains(row);
      });
    }

    m_runtimeNgSelectionDirty = true;
    invalidateResult();
  });

  watcher->setFuture(QtConcurrent::run([source, segmentId, rowsToLoad, epoch]() -> RuntimeNeuroglancerChunkBatchResult {
    RuntimeNeuroglancerChunkBatchResult out;
    out.epoch = epoch;
    CHECK(source);
    for (const uint32_t row : rowsToLoad) {
      try {
        auto chunkMesh = source->loadChunkMeshBlocking(segmentId, row);
        CHECK(chunkMesh);
        out.loadedRows.push_back(row);
        out.loadedChunks.push_back(std::move(chunkMesh));
      }
      catch (const std::exception& e) {
        out.failedRows.push_back(row);
        if (out.error.isEmpty()) {
          out.error = QString::fromUtf8(e.what());
        }
      }
    }
    return out;
  }));
}

void Z3DMeshFilter::applyRuntimeNeuroglancerSelection()
{
  if (m_runtimeNgExportActive || !m_runtimeNgProgressiveRendering) {
    return;
  }

  if (!m_runtimeNgSource || !m_runtimeNgManifest || !m_runtimeNgSelectionDirty) {
    return;
  }

  m_runtimeNgSelectionDirty = false;

  if (!m_runtimeNgBaseReady) {
    getVisibleData();
    if (m_meshList != m_origMeshList) {
      m_dataIsInvalid = true;
      invalidateResult();
    }
    return;
  }

  const glm::uvec4 currentViewport = viewport();
  if (currentViewport.z == 0U || currentViewport.w == 0U) {
    return;
  }

  const float detailCutoff =
    m_runtimeNgInteractionActive ? kRuntimeNgInteractionDetailCutoff : kRuntimeNgIdleDetailCutoff;
  const glm::mat4 modelViewProjection =
    m_globalParameters.camera.get().projectionViewMatrix(MonoEye) * coordTransform();
  const auto clippingPlanes = ZNeuroglancerPrecomputedMeshSource::getFrustumPlanes(modelViewProjection);

  const auto desiredRows = ZNeuroglancerPrecomputedMeshSource::desiredChunksForView(*m_runtimeNgManifest,
                                                                                    modelViewProjection,
                                                                                    clippingPlanes,
                                                                                    detailCutoff,
                                                                                    currentViewport.z,
                                                                                    currentViewport.w);

  std::vector<uint32_t> rowsToLoad;
  rowsToLoad.reserve(desiredRows.size());
  for (const auto& desired : desiredRows) {
    if (desired.empty || m_runtimeNgLoadedRows.contains(desired.row) || m_runtimeNgRowsInFlight.contains(desired.row) ||
        m_runtimeNgFailedRows.contains(desired.row)) {
      continue;
    }
    rowsToLoad.push_back(desired.row);
  }
  if (!rowsToLoad.empty()) {
    requestRuntimeNeuroglancerRows(rowsToLoad);
  }

  const auto drawChunks = ZNeuroglancerPrecomputedMeshSource::chunksToDrawForView(
    *m_runtimeNgManifest,
    modelViewProjection,
    clippingPlanes,
    detailCutoff,
    currentViewport.z,
    currentViewport.w,
    [this](uint32_t /*lod*/, uint32_t row, float /*renderScale*/) {
      return m_runtimeNgLoadedRows.contains(row);
    });

  std::vector<ZMesh*> newVisibleMeshes;
  for (const auto& drawChunk : drawChunks) {
    auto it = m_runtimeNgLoadedRows.find(drawChunk.row);
    if (it == m_runtimeNgLoadedRows.end()) {
      continue;
    }
    const auto& chunkMesh = it->second;
    CHECK(chunkMesh);
    CHECK(drawChunk.subChunkBegin <= drawChunk.subChunkEnd);
    CHECK(drawChunk.subChunkEnd <= chunkMesh->subMeshes.size());
    for (uint32_t i = drawChunk.subChunkBegin; i < drawChunk.subChunkEnd; ++i) {
      const auto& subMesh = chunkMesh->subMeshes[i];
      if (subMesh && !subMesh->empty()) {
        newVisibleMeshes.push_back(subMesh.get());
      }
    }
  }

  if (newVisibleMeshes != m_runtimeNgVisibleMeshes) {
    m_runtimeNgVisibleMeshes = std::move(newVisibleMeshes);
    getVisibleData();
    m_dataIsInvalid = true;
    invalidateResult();
  }
}

bool Z3DMeshFilter::hasRuntimeNeuroglancerLod() const
{
  return m_runtimeNgSource != nullptr && m_runtimeNgManifest != nullptr && m_runtimeNgBaseReady;
}

bool Z3DMeshFilter::isSameRuntimeNeuroglancerSource(const ZNeuroglancerMeshExternalSourceKey& key) const
{
  if (!m_runtimeNgSourceKey) {
    return false;
  }
  const auto& current = *m_runtimeNgSourceKey;
  return current.rootUrl == key.rootUrl && current.meshSourceDirUrl == key.meshSourceDirUrl &&
         current.segmentId == key.segmentId && sameOptionalArray(current.baseResolutionNm, key.baseResolutionNm) &&
         sameOptionalArray(current.baseVoxelOffset, key.baseVoxelOffset);
}

void Z3DMeshFilter::getVisibleData()
{
  if (m_runtimeNgExportActive) {
    m_meshList = m_runtimeNgFrozenVisibleMeshes;
    return;
  }
  if (hasRuntimeNeuroglancerLod()) {
    m_meshList = m_runtimeNgVisibleMeshes;
    return;
  }
  m_meshList = m_origMeshList;
}

} // namespace nim
