#include "z3dvolumeraycasterrenderer.h"

#include "z3dtexture.h"
#include "z3dvolume.h"
#include "z3dimg.h"
#include <absl/strings/str_cat.h>

namespace nim {

Z3DVolumeRaycasterRenderer::Z3DVolumeRaycasterRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_is2DImage(false)
  , m_entryTexCoordTexture(nullptr)
  , m_entryEyeCoordTexture(nullptr)
  , m_exitTexCoordTexture(nullptr)
  , m_exitEyeCoordTexture(nullptr)
  , m_VAO(1)
{
  // m_gradientMode.addOptions("None", "Forward Differences", "Central Differences", "Filtered");
  // m_gradientMode.select("None");
  //  todo: add gradient
  // addParameter(m_gradientMode);
  //  m_raycasterShader.loadFromSourceFile("pass.vert", "volume_raycaster.frag",
  //                                       m_rendererBase.generateHeader() + generateHeader());
  //  m_2dImageShader.loadFromSourceFile("transform_with_2dtexture.vert", "image2d_with_transfun.frag",
  //                                     m_rendererBase.generateHeader() + generateHeader());
  //  m_volumeSliceWithTransferfunShader.loadFromSourceFile("transform_with_3dtexture.vert",
  //  "volume_slice_with_transfun.frag",
  //                                                        m_rendererBase.generateHeader() + generateHeader());

  m_scRaycasterShader.loadFromSourceFile("pass.vert",
                                         "volume_raycaster_single_channel.frag",
                                         m_rendererBase.generateHeader() + generateHeader());
  m_sc2dImageShader.loadFromSourceFile("transform_with_2dtexture.vert",
                                       "image2d_with_transfun_single_channel.frag",
                                       m_rendererBase.generateHeader() + generateHeader());
  m_scVolumeSliceWithTransferfunShader.loadFromSourceFile("transform_with_3dtexture.vert",
                                                          "volume_slice_with_transfun_single_channel.frag",
                                                          m_rendererBase.generateHeader() + generateHeader());
  m_mergeChannelShader.loadFromSourceFile("pass.vert",
                                          "image2d_array_compositor.frag",
                                          m_rendererBase.generateHeader() + generateHeader());
}

void Z3DVolumeRaycasterRenderer::setChannels(const std::vector<std::unique_ptr<Z3DVolume>>& volsIn)
{
  std::vector<Z3DVolume*> vols;
  for (size_t i = 0; i < volsIn.size(); ++i) {
    vols.push_back(volsIn[i].get());
  }

  if (m_volumes != vols) {
    bool numChannelsChanged = m_volumes.size() != vols.size();
    if (numChannelsChanged) {
      m_volumeUniformNames.clear();
      m_volumeDimensionNames.clear();
      m_transferFuncUniformNames.clear();
    }

    m_volumes = vols;

    if (numChannelsChanged) {
      for (size_t i = 0; i < m_volumes.size(); ++i) {
        m_volumeUniformNames.push_back(QString("volume_%1").arg(i + 1));
        m_volumeDimensionNames.push_back(QString("volume_dimensions_%1").arg(i + 1));
        m_transferFuncUniformNames.push_back(QString("transfer_function_%1").arg(i + 1));
      }
      setChannelCount(m_volumes.size());
    }

    m_is2DImage = !m_volumes.empty() && m_volumes[0]->is2DData();

    if (numChannelsChanged) {
      compile();
    }
  }
}

void Z3DVolumeRaycasterRenderer::setChannels(const Z3DImg& img)
{
  setChannels(img.volumes());
}

void Z3DVolumeRaycasterRenderer::setChannelCount(size_t count)
{
  m_channelVisibilities.assign(count, false);
  m_transferFunctions.assign(count, nullptr);
  m_texFilterModes.assign(count, GL_LINEAR);
}

void Z3DVolumeRaycasterRenderer::setChannelVisibility(size_t index, bool visible)
{
  CHECK_LT(index, m_channelVisibilities.size());
  if (m_channelVisibilities[index] == visible) {
    return;
  }
  m_channelVisibilities[index] = visible;
  compile();
}

void Z3DVolumeRaycasterRenderer::setChannelVisibilities(const std::vector<bool>& visibilities)
{
  CHECK_EQ(m_channelVisibilities.size(), visibilities.size());
  if (m_channelVisibilities == visibilities) {
    return;
  }
  m_channelVisibilities = visibilities;
  compile();
}

void Z3DVolumeRaycasterRenderer::setTransferFunction(size_t index, Z3DTransferFunction* transferFunction)
{
  CHECK_LT(index, m_transferFunctions.size());
  m_transferFunctions[index] = transferFunction;
}

void Z3DVolumeRaycasterRenderer::setTransferFunctions(const std::vector<Z3DTransferFunction*>& transferFunctions)
{
  CHECK_EQ(m_transferFunctions.size(), transferFunctions.size());
  m_transferFunctions = transferFunctions;
}

void Z3DVolumeRaycasterRenderer::setTexFilterMode(size_t index, GLint mode)
{
  CHECK_LT(index, m_texFilterModes.size());
  m_texFilterModes[index] = mode;
}

void Z3DVolumeRaycasterRenderer::setTexFilterModes(const std::vector<GLint>& modes)
{
  CHECK_EQ(m_texFilterModes.size(), modes.size());
  m_texFilterModes = modes;
}

void Z3DVolumeRaycasterRenderer::addQuad(const ZMesh& quad)
{
  if (quad.empty() || (quad.numVertices() != 4 && quad.numVertices() != 6) ||
      (quad.numVertices() != quad.num2DTextureCoordinates() && quad.numVertices() != quad.num3DTextureCoordinates())) {
    LOG(ERROR) << "Input quad should be 2D slice with either 2D or 3D texture coordinates";
    return;
  }
  m_quads.push_back(quad);
  m_entryTexCoordTexture = nullptr;
  m_entryEyeCoordTexture = nullptr;
  m_exitTexCoordTexture = nullptr;
  m_exitEyeCoordTexture = nullptr;
}

void Z3DVolumeRaycasterRenderer::setEntryExitInfo(const Z3DTexture* entryTexCoordTexture,
                                                  const Z3DTexture* entryEyeCoordTexture,
                                                  const Z3DTexture* exitTexCoordTexture,
                                                  const Z3DTexture* exitEyeCoordTexture)
{
  m_entryTexCoordTexture = entryTexCoordTexture;
  m_entryEyeCoordTexture = entryEyeCoordTexture;
  m_exitTexCoordTexture = exitTexCoordTexture;
  m_exitEyeCoordTexture = exitEyeCoordTexture;
  m_quads.clear();
}

void Z3DVolumeRaycasterRenderer::bindVolumesAndTransferFuncs(Z3DShaderProgram& shader)
{
  shader.setLogUniformLocationError(false);

  size_t idx = 0;
  for (size_t i = 0; i < m_volumes.size(); ++i) {
    if (!m_channelVisibilities[i]) {
      continue;
    }
    CHECK(m_transferFunctions[i] != nullptr);

    // volumes
    shader.bindTexture(m_volumeUniformNames[idx], m_volumes[i]->texture(), m_texFilterModes[i], m_texFilterModes[i]);
    shader.setUniform(m_volumeDimensionNames[idx], glm::vec3(m_volumes[i]->dimensions()));

    // transfer functions
    shader.bindTexture(m_transferFuncUniformNames[idx++], m_transferFunctions[i]->texture());
  }

  shader.setLogUniformLocationError(true);
}

void Z3DVolumeRaycasterRenderer::bindVolumeAndTransferFunc(Z3DShaderProgram& shader, size_t idx)
{
  shader.setLogUniformLocationError(false);

  shader.bindTexture(m_volumeUniformNames[0], m_volumes[idx]->texture(), m_texFilterModes[idx], m_texFilterModes[idx]);
  shader.setUniform(m_volumeDimensionNames[0], glm::vec3(m_volumes[idx]->dimensions()));

  // transfer functions
  CHECK(idx < m_transferFunctions.size());
  CHECK(m_transferFunctions[idx] != nullptr);
  shader.bindTexture(m_transferFuncUniformNames[0], m_transferFunctions[idx]->texture());

  shader.setLogUniformLocationError(true);
}

void Z3DVolumeRaycasterRenderer::compile()
{
  //  m_raycasterShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  //  m_2dImageShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  //  m_volumeSliceWithTransferfunShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());

  m_scRaycasterShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_sc2dImageShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_scVolumeSliceWithTransferfunShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_mergeChannelShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
}

std::string Z3DVolumeRaycasterRenderer::generateHeader()
{
  std::string header;
  header.reserve(384);

  if (hasVisibleRendering()) {
    size_t numVisibleChannels = 0;
    for (size_t i = 0; i < m_volumes.size(); ++i) {
      if (m_channelVisibilities[i]) {
        ++numVisibleChannels;
      }
    }
    fmt::format_to(std::back_inserter(header), "#define NUM_VOLUMES {}\n", numVisibleChannels);
  } else {
    absl::StrAppend(&header, "#define NUM_VOLUMES 0\n", "#define DISABLE_TEXTURE_COORD_OUTPUT\n");
  }

  const bool useMIPMerge = m_compositingModeValue == VolumeCompositingMode::MaximumIntensityProjection ||
                           m_compositingModeValue == VolumeCompositingMode::LocalMIP ||
                           m_compositingModeValue == VolumeCompositingMode::MIPOpaque ||
                           m_compositingModeValue == VolumeCompositingMode::LocalMIPOpaque;

  switch (m_compositingModeValue) {
    case VolumeCompositingMode::DirectVolumeRendering:
      absl::StrAppend(&header,
                      "#define COMPOSITING(result, color, currentRayLength, rayDepth) ",
                      "compositeDVR(result, color, currentRayLength, rayDepth);\n");
      break;
    case VolumeCompositingMode::IsoSurface:
      absl::StrAppend(&header,
                      "#define ISO\n",
                      "#define COMPOSITING(result, color, currentRayLength, rayDepth) ",
                      "compositeISO(result, color, currentRayLength, rayDepth, iso_value);\n");
      break;
    case VolumeCompositingMode::MaximumIntensityProjection:
      absl::StrAppend(&header, "#define MIP\n");
      break;
    case VolumeCompositingMode::LocalMIP:
      absl::StrAppend(&header, "#define MIP\n", "#define LOCAL_MIP\n");
      break;
    case VolumeCompositingMode::XRay:
      absl::StrAppend(&header,
                      "#define COMPOSITING(result, color, currentRayLength, rayDepth) ",
                      "compositeXRay(result, color, currentRayLength, rayDepth);\n");
      break;
    case VolumeCompositingMode::MIPOpaque:
      absl::StrAppend(&header, "#define MIP\n", "#define RESULT_OPAQUE\n");
      break;
    case VolumeCompositingMode::LocalMIPOpaque:
      absl::StrAppend(&header, "#define MIP\n", "#define LOCAL_MIP\n", "#define RESULT_OPAQUE\n");
      break;
  }

  if (!m_quads.empty() || useMIPMerge) {
    absl::StrAppend(&header, "#define MAX_PROJ_MERGE\n");
  }

  return header;
}

void Z3DVolumeRaycasterRenderer::render(Z3DEye eye)
{
  if (!hasVisibleRendering()) {
    return;
  }

  if (m_quads.empty()) {
    if (m_entryTexCoordTexture == nullptr || m_entryEyeCoordTexture == nullptr || m_exitTexCoordTexture == nullptr ||
        m_exitEyeCoordTexture == nullptr) {
      return;
    }
  } else {
    for (size_t i = 0; i < m_quads.size(); ++i) {
      if (m_is2DImage && m_quads[i].numVertices() != m_quads[i].num2DTextureCoordinates()) {
        return;
      }
      if (!m_is2DImage && m_quads[i].numVertices() != m_quads[i].num3DTextureCoordinates()) {
        return;
      }
    }
  }

  std::vector<size_t> visibleIdxs;
  for (size_t i = 0; i < m_volumes.size(); ++i) {
    if (m_channelVisibilities[i]) {
      visibleIdxs.push_back(i);
    }
  }

  if (!m_quads.empty()) { // 2d image or slice from 3d volume
    if (m_is2DImage) { // image is 2D
      m_sc2dImageShader.bind();
      m_rendererBase.setGlobalShaderParameters(m_sc2dImageShader, eye);

      if (visibleIdxs.size() == 1) {
        bindVolumeAndTransferFunc(m_sc2dImageShader, visibleIdxs[0]);

        for (size_t i = 0; i < m_quads.size(); ++i) {
          renderTriangleList(m_VAO, m_sc2dImageShader, m_quads[i]);
        }

      } else {
        for (size_t j = 0; j < visibleIdxs.size(); ++j) {
          m_layerTarget->attachSlice(j);
          m_layerTarget->bind();
          m_layerTarget->clear();
          bindVolumeAndTransferFunc(m_sc2dImageShader, visibleIdxs[j]);

          for (size_t i = 0; i < m_quads.size(); ++i) {
            renderTriangleList(m_VAO, m_sc2dImageShader, m_quads[i]);
          }

          m_layerTarget->release();
        }
      }

      m_sc2dImageShader.release();
    } else { // image is 3D, but a 2D slice will be shown
      m_scVolumeSliceWithTransferfunShader.bind();
      m_rendererBase.setGlobalShaderParameters(m_scVolumeSliceWithTransferfunShader, eye);

      if (visibleIdxs.size() == 1) {
        bindVolumeAndTransferFunc(m_scVolumeSliceWithTransferfunShader, visibleIdxs[0]);

        for (size_t i = 0; i < m_quads.size(); ++i) {
          renderTriangleList(m_VAO, m_scVolumeSliceWithTransferfunShader, m_quads[i]);
        }
      } else {
        for (size_t j = 0; j < visibleIdxs.size(); ++j) {
          m_layerTarget->attachSlice(j);
          m_layerTarget->bind();
          m_layerTarget->clear();

          bindVolumeAndTransferFunc(m_scVolumeSliceWithTransferfunShader, visibleIdxs[j]);

          for (size_t i = 0; i < m_quads.size(); ++i) {
            renderTriangleList(m_VAO, m_scVolumeSliceWithTransferfunShader, m_quads[i]);
          }

          m_layerTarget->release();
        }
      }

      m_scVolumeSliceWithTransferfunShader.release();
    }
  } else { // 3d volume raycasting
    m_scRaycasterShader.bind();

    m_rendererBase.setGlobalShaderParameters(m_scRaycasterShader, eye);

    float n = m_rendererBase.camera().nearDist();
    float f = m_rendererBase.camera().farDist();
    // http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
    //  zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
    float a = f * n / (f - n);
    float b = 0.5f * (f + n) / (f - n) + 0.5f;
    m_scRaycasterShader.setUniform("ze_to_zw_b", b);
    m_scRaycasterShader.setUniform("ze_to_zw_a", a);

    // entry exit points
    m_scRaycasterShader.bindTexture("ray_entry_tex_coord", m_entryTexCoordTexture);
    m_scRaycasterShader.bindTexture("ray_entry_eye_coord", m_entryEyeCoordTexture);
    m_scRaycasterShader.bindTexture("ray_exit_tex_coord", m_exitTexCoordTexture);
    m_scRaycasterShader.bindTexture("ray_exit_eye_coord", m_exitEyeCoordTexture);

    if (m_compositingModeValue == VolumeCompositingMode::IsoSurface) {
      m_scRaycasterShader.setUniform("iso_value", m_isoValue);
    }

    if (m_compositingModeValue == VolumeCompositingMode::LocalMIP ||
        m_compositingModeValue == VolumeCompositingMode::LocalMIPOpaque) {
      m_scRaycasterShader.setUniform("local_MIP_threshold", m_localMIPThreshold);
    }

    m_scRaycasterShader.setUniform("sampling_rate", m_samplingRateValue);

    if (visibleIdxs.size() == 1) {
      bindVolumeAndTransferFunc(m_scRaycasterShader, visibleIdxs[0]);
      renderScreenQuad(m_VAO, m_scRaycasterShader);
    } else {
      for (size_t i = 0; i < visibleIdxs.size(); ++i) {
        m_layerTarget->attachSlice(i);
        m_layerTarget->bind();
        m_layerTarget->clear();

        bindVolumeAndTransferFunc(m_scRaycasterShader, visibleIdxs[i]);
        renderScreenQuad(m_VAO, m_scRaycasterShader);

        m_layerTarget->release();
      }
    }

    m_scRaycasterShader.release();
  }

  if (visibleIdxs.size() > 1) {
    m_mergeChannelShader.bind();
    m_mergeChannelShader.bindTexture("color_texture", m_layerTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader.bindTexture("depth_texture", m_layerTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(m_VAO, m_mergeChannelShader);
    m_mergeChannelShader.release();
  }

  CHECK_GL_ERROR
}

void Z3DVolumeRaycasterRenderer::renderPicking(Z3DEye) {}

void Z3DVolumeRaycasterRenderer::translate(double dx, double dy, double dz)
{
  for (auto vol : m_volumes) {
    if (vol) {
      vol->translate(dx, dy, dz);
    }
  }
}

bool Z3DVolumeRaycasterRenderer::hasVisibleRendering() const
{
  for (size_t i = 0; i < m_volumes.size(); ++i) {
    if (m_channelVisibilities[i]) {
      return true;
    }
  }
  return false;
}

} // namespace nim
