#include "z3dimgraycasterrenderer.h"

#include "z3dtexture.h"
#include "z3dvolume.h"
#include "z3dimg.h"
#include <tbb/parallel_for.h>
#include <tbb/concurrent_unordered_set.h>

namespace nim {

Z3DImgRaycasterRenderer::Z3DImgRaycasterRenderer(Z3DRendererBase &rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_samplingRate("Sampling Rate", 2.f, 0.01f, 20.f)
  , m_isoValue("ISO Value", 0.5f, 0.0f, 1.0f)
  , m_localMIPThreshold("Local MIP Threshold", 0.8f, 0.01f, 1.f)
  , m_compositingMode("Compositing")
  , m_entryTexCoordTexture(nullptr)
  , m_entryEyeCoordTexture(nullptr)
  , m_exitTexCoordTexture(nullptr)
  , m_exitEyeCoordTexture(nullptr)
  , m_opaque(false)
  , m_alpha(1.0)
  , m_VAO(1)
{
  //m_gradientMode.addOptions("None", "Forward Differences", "Central Differences", "Filtered");
  //m_gradientMode.select("None");
  // todo: add gradient
  //addParameter(m_gradientMode);

  // compositing modes
  m_compositingMode.addOptions("Direct Volume Rendering", "Maximum Intensity Projection",
                               "MIP Opaque", "Local MIP", "Local MIP Opaque", "ISO Surface", "X Ray");
  m_compositingMode.select("MIP Opaque");

  connect(&m_compositingMode, SIGNAL(valueChanged()), this, SLOT(adjustWidgets()));
  connect(&m_compositingMode, SIGNAL(valueChanged()), this, SLOT(compile()));
  //connect(&m_gradientMode, SIGNAL(valueChanged()), this, SLOT(compile()));

  adjustWidgets();

  //  m_raycasterShader.bindFragDataLocation(0, "FragData0");
  //  m_raycasterShader.loadFromSourceFile("pass.vert", "volume_raycaster.frag",
  //                                       m_rendererBase.generateHeader() + generateHeader());
  //  m_2dImageShader.bindFragDataLocation(0, "FragData0");
  //  m_2dImageShader.loadFromSourceFile("transform_with_2dtexture.vert", "image2d_with_transfun.frag",
  //                                     m_rendererBase.generateHeader() + generateHeader());
  //  m_volumeSliceWithTransferfunShader.bindFragDataLocation(0, "FragData0");
  //  m_volumeSliceWithTransferfunShader.loadFromSourceFile("transform_with_3dtexture.vert", "volume_slice_with_transfun.frag",
  //                                                        m_rendererBase.generateHeader() + generateHeader());

  m_scRaycasterShader.bindFragDataLocation(0, "FragData0");
  m_scRaycasterShader.loadFromSourceFile("pass.vert", "volume_raycaster_single_channel.frag",
                                         m_rendererBase.generateHeader() + generateHeader());
  m_sc2dImageShader.bindFragDataLocation(0, "FragData0");
  m_sc2dImageShader.loadFromSourceFile("transform_with_2dtexture.vert", "image2d_with_transfun_single_channel.frag",
                                       m_rendererBase.generateHeader() + generateHeader());
  m_scVolumeSliceWithTransferfunShader.bindFragDataLocation(0, "FragData0");
  m_scVolumeSliceWithTransferfunShader.loadFromSourceFile("transform_with_3dtexture.vert", "volume_slice_with_transfun_single_channel.frag",
                                                          m_rendererBase.generateHeader() + generateHeader());
  m_scFullResRaycasterShader.bindFragDataLocation(0, "FragData0");
  m_scFullResRaycasterShader.bindFragDataLocation(1, "FragData1");
  m_scFullResRaycasterShader.bindFragDataLocation(2, "FragData2");
  m_scFullResRaycasterShader.bindFragDataLocation(3, "FragData3");
  m_scFullResRaycasterShader.bindFragDataLocation(4, "FragData4");
  m_scFullResRaycasterShader.loadFromSourceFile("pass.vert", "image3d_raycaster.frag",
                                                m_rendererBase.generateHeader() + generateHeader());
  m_mergeChannelShader.bindFragDataLocation(0, "FragData0");
  m_mergeChannelShader.loadFromSourceFile("pass.vert", "image2d_array_compositor.frag",
                                          m_rendererBase.generateHeader() + generateHeader());
  CHECK_GL_ERROR;
}

QString Z3DImgRaycasterRenderer::compositeMode() const
{
  return m_compositingMode.get();
}

void Z3DImgRaycasterRenderer::setData(Z3DImg &img)
{
  m_img = &img;

  if (m_img->numChannels() != m_volumeUniformNames.size()) {
    m_volumeUniformNames.clear();
    m_volumeDimensionNames.clear();
    m_transferFuncUniformNames.clear();
    m_channelVisibleParas.clear();
    m_transferFuncParas.clear();
    m_texFilterModeParas.clear();
    for (size_t i=0; i<m_img->numChannels(); ++i) {
      m_volumeUniformNames.push_back(QString("volume_%1").arg(i+1));
      m_volumeDimensionNames.push_back(QString("volume_dimensions_%1").arg(i+1));
      m_transferFuncUniformNames.push_back(QString("transfer_function_%1").arg(i+1));
      m_channelVisibleParas.emplace_back(std::make_unique<ZBoolParameter>(QString("Show Channel %1").arg(i+1), true));
      connect(m_channelVisibleParas[i].get(), SIGNAL(valueChanged()), this, SLOT(compile()));
      m_transferFuncParas.emplace_back(std::make_unique<Z3DTransferFunctionParameter>(QString("Transfer Function %1").arg(i+1)));
      m_transferFuncParas[i]->setVolume(m_img->volumes().at(i).get());
      m_texFilterModeParas.emplace_back(std::make_unique<ZStringIntOptionParameter>(QString("Texture Filtering %1").arg(i+1)));
      m_texFilterModeParas[i]->addOptionsWithData(qMakePair(QString("Nearest"), static_cast<int>(GL_NEAREST)),
                                                  qMakePair(QString("Linear"), static_cast<int>(GL_LINEAR)));
      m_texFilterModeParas[i]->select("Linear");
    }
  }
  compile();
  resetTransferFunctions();
}

void Z3DImgRaycasterRenderer::addQuad(const ZMesh &quad)
{
  if (quad.empty() ||
      (quad.numVertices() != 4 && quad.numVertices() != 6) ||
      (quad.numVertices() != quad.num2DTextureCoordinates() &&
       quad.numVertices() != quad.num3DTextureCoordinates())) {
    LERROR() << "Input quad should be 2D slice with either 2D or 3D texture coordinates";
    return;
  }
  m_quads.push_back(quad);
  m_entryTexCoordTexture = nullptr;
  m_entryEyeCoordTexture = nullptr;
  m_exitTexCoordTexture = nullptr;
  m_exitEyeCoordTexture = nullptr;
}

void Z3DImgRaycasterRenderer::setEntryExitInfo(const Z3DTexture *entryTexCoordTexture, const Z3DTexture *entryEyeCoordTexture,
                                               const Z3DTexture *exitTexCoordTexture, const Z3DTexture *exitEyeCoordTexture)
{
  m_entryTexCoordTexture = entryTexCoordTexture;
  m_entryEyeCoordTexture = entryEyeCoordTexture;
  m_exitTexCoordTexture = exitTexCoordTexture;
  m_exitEyeCoordTexture = exitEyeCoordTexture;
  m_quads.clear();
}


void Z3DImgRaycasterRenderer::adjustWidgets()
{
  m_isoValue.setVisible(m_compositingMode.isSelected("ISO Surface"));
  m_localMIPThreshold.setVisible(m_compositingMode.isSelected("Local MIP") ||
                                 m_compositingMode.isSelected("Local MIP Opaque"));
}

void Z3DImgRaycasterRenderer::bindVolumesAndTransferFuncs(Z3DShaderProgram &shader)
{
  shader.setLogUniformLocationError(false);

  size_t idx = 0;
  for (size_t i=0; i<m_img->numChannels(); ++i) {
    if (!m_channelVisibleParas[i]->get())
      continue;

    // volumes
    shader.bindTexture(m_volumeUniformNames[idx], m_img->volumes().at(i)->texture(), m_texFilterModeParas[i]->associatedData(),
                       m_texFilterModeParas[i]->associatedData());
    shader.setUniform(m_volumeDimensionNames[idx], glm::vec3(m_img->volumes().at(i)->dimensions()));

    // transfer functions
    shader.bindTexture(m_transferFuncUniformNames[idx++], m_transferFuncParas[i]->get().texture());

    CHECK_GL_ERROR;
  }

  shader.setLogUniformLocationError(true);
}

void Z3DImgRaycasterRenderer::bindVolumeAndTransferFunc(Z3DShaderProgram &shader, size_t idx)
{
  shader.setLogUniformLocationError(false);

  shader.bindTexture(m_volumeUniformNames[0], m_img->volumes().at(idx)->texture(), m_texFilterModeParas[idx]->associatedData(),
      m_texFilterModeParas[idx]->associatedData());
  shader.setUniform(m_volumeDimensionNames[0], glm::vec3(m_img->volumes().at(idx)->dimensions()));

  // transfer functions
  shader.bindTexture(m_transferFuncUniformNames[0], m_transferFuncParas[idx]->get().texture());

  CHECK_GL_ERROR;

  shader.setLogUniformLocationError(true);
}

void Z3DImgRaycasterRenderer::compile()
{
  //  m_raycasterShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  //  m_2dImageShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  //  m_volumeSliceWithTransferfunShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());

  m_scRaycasterShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_sc2dImageShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_scVolumeSliceWithTransferfunShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_scFullResRaycasterShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_mergeChannelShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
}

QString Z3DImgRaycasterRenderer::generateHeader()
{
  QString headerSource;

  size_t numVisibleChannels = 0;
  size_t numLevels = 1;
  if (m_img) {
    for (size_t i=0; i<m_img->numChannels(); ++i) {
      if (m_channelVisibleParas[i]->get()) {
        ++numVisibleChannels;
      }
    }
    numLevels = m_img->numLevels();
  }

  headerSource += QString("#define LEVEL_COUNT %1\n").arg(numLevels);

  if (numVisibleChannels > 0) {
    headerSource += QString("#define NUM_VOLUMES %1\n").arg(numVisibleChannels);
  } else {
    headerSource += QString("#define NUM_VOLUMES 0\n");
    headerSource += "#define DISABLE_TEXTURE_COORD_OUTPUT\n";
  }

  //  if (!m_gradientMode.isSelected("None"))
  //    headerSource += "#define USE_GRADIENTS\n";

  if (m_compositingMode.isSelected("Direct Volume Rendering")) {
    headerSource += "#define COMPOSITING(result, color, currentRayLength, rayDepth) ";
    headerSource += "compositeDVR(result, color, currentRayLength, rayDepth);\n";
  } else if (m_compositingMode.isSelected("ISO Surface")) {
    headerSource += "#define ISO\n";
    headerSource += "#define COMPOSITING(result, color, currentRayLength, rayDepth) ";
    headerSource += "compositeISO(result, color, currentRayLength, rayDepth, iso_value);\n";
  } else if (m_compositingMode.isSelected("Maximum Intensity Projection")) {
    headerSource += "#define MIP\n";
  } else if (m_compositingMode.isSelected("Local MIP")) {
    headerSource += "#define MIP\n";
    headerSource += "#define LOCAL_MIP\n";
  } else if (m_compositingMode.isSelected("X Ray")) {
    headerSource += "#define COMPOSITING(result, color, currentRayLength, rayDepth) ";
    headerSource += "compositeXRay(result, color, currentRayLength, rayDepth);\n";
  } else if (m_compositingMode.isSelected("MIP Opaque")) {
    headerSource += "#define MIP\n";
    headerSource += "#define RESULT_OPAQUE\n";
  } else if (m_compositingMode.isSelected("Local MIP Opaque")) {
    headerSource += "#define MIP\n";
    headerSource += "#define LOCAL_MIP\n";
    headerSource += "#define RESULT_OPAQUE\n";
  }

  if (!m_quads.empty() ||
      m_compositingMode.isSelected("Maximum Intensity Projection") ||
      m_compositingMode.isSelected("Local MIP") ||
      m_compositingMode.isSelected("MIP Opaque") ||
      m_compositingMode.isSelected("Local MIP Opaque")) {
    headerSource += "#define MAX_PROJ_MERGE\n";
  }

  return headerSource;
}

void Z3DImgRaycasterRenderer::render(Z3DEye eye)
{
  if (!hasVisibleRendering())
    return;

  if (m_quads.empty()) {
    if (m_entryTexCoordTexture == nullptr || m_entryEyeCoordTexture == nullptr ||
        m_exitTexCoordTexture == nullptr || m_exitEyeCoordTexture == nullptr)
      return;
  } else {
    for (size_t i=0; i<m_quads.size(); ++i) {
      if (m_img->is2DData() && m_quads[i].numVertices() != m_quads[i].num2DTextureCoordinates())
        return;
      if (m_img->is3DData() && m_quads[i].numVertices() != m_quads[i].num3DTextureCoordinates())
        return;
    }
  }

  std::vector<size_t> visibleIdxs;
  for (size_t i=0; i<m_img->numChannels(); ++i) {
    if (m_channelVisibleParas[i]->get()) {
      visibleIdxs.push_back(i);
    }
  }
  bool fullResRendering = false;

  if (!m_quads.empty()) { // 2d image or slice from 3d volume
    if (m_img->is2DData()) {   // image is 2D
      m_sc2dImageShader.bind();
      m_rendererBase.setGlobalShaderParameters(m_sc2dImageShader, eye);

      if (visibleIdxs.size() == 1) {
        bindVolumeAndTransferFunc(m_sc2dImageShader, visibleIdxs[0]);

        for (size_t i=0; i<m_quads.size(); ++i)
          renderTriangleList(m_VAO, m_sc2dImageShader, m_quads[i]);

      } else {
        for (size_t j=0; j<visibleIdxs.size(); ++j) {
          m_layerTarget->attachSlice(j);
          m_layerTarget->bind();
          m_layerTarget->clear();
          bindVolumeAndTransferFunc(m_sc2dImageShader, visibleIdxs[j]);

          for (size_t i=0; i<m_quads.size(); ++i)
            renderTriangleList(m_VAO, m_sc2dImageShader, m_quads[i]);

          m_layerTarget->release();
        }
      }

      m_sc2dImageShader.release();
    } else {   // image is 3D, but a 2D slice will be shown
      m_scVolumeSliceWithTransferfunShader.bind();
      m_rendererBase.setGlobalShaderParameters(m_scVolumeSliceWithTransferfunShader, eye);

      if (visibleIdxs.size() == 1) {
        bindVolumeAndTransferFunc(m_scVolumeSliceWithTransferfunShader, visibleIdxs[0]);

        for (size_t i=0; i<m_quads.size(); ++i)
          renderTriangleList(m_VAO, m_scVolumeSliceWithTransferfunShader, m_quads[i]);
      } else {
        for (size_t j=0; j<visibleIdxs.size(); ++j) {
          m_layerTarget->attachSlice(j);
          m_layerTarget->bind();
          m_layerTarget->clear();

          bindVolumeAndTransferFunc(m_scVolumeSliceWithTransferfunShader, visibleIdxs[j]);

          for (size_t i=0; i<m_quads.size(); ++i)
            renderTriangleList(m_VAO, m_scVolumeSliceWithTransferfunShader, m_quads[i]);

          m_layerTarget->release();
        }
      }

      m_scVolumeSliceWithTransferfunShader.release();
    }
  } else {  // 3d volume raycasting
    if (m_img->isVolumeDownsampled()) {
      fullResRendering = true;
      m_scFullResRaycasterShader.bind();

      float n = m_rendererBase.camera().nearDist();
      float f = m_rendererBase.camera().farDist();
      //http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
      // zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
      float a = f*n/(f-n);
      float b = 0.5f * (f+n)/(f-n) + 0.5f;
      m_scFullResRaycasterShader.setUniform("minus_near_dist", -n);
      LINFO() << -n;
      m_scFullResRaycasterShader.setUniform("ze_to_zw_b", b);
      m_scFullResRaycasterShader.setUniform("ze_to_zw_a", a);

      // entry exit points
      m_scFullResRaycasterShader.bindTexture("ray_entry_tex_coord", m_entryTexCoordTexture);
      m_scFullResRaycasterShader.bindTexture("ray_entry_eye_coord", m_entryEyeCoordTexture);
      m_scFullResRaycasterShader.bindTexture("ray_exit_tex_coord",  m_exitTexCoordTexture);
      m_scFullResRaycasterShader.bindTexture("ray_exit_eye_coord", m_exitEyeCoordTexture);

      if (m_compositingMode.get() ==  "ISO Surface")
        m_scFullResRaycasterShader.setUniform("iso_value", m_isoValue.get());

      if (m_compositingMode.get() ==  "Local MIP" || m_compositingMode.get() ==  "Local MIP Opaque")
        m_scFullResRaycasterShader.setUniform("local_MIP_threshold", m_localMIPThreshold.get());

      m_scFullResRaycasterShader.setUniform("sampling_rate", m_samplingRate.get());

      // render first channel
      GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0,
                                GL_COLOR_ATTACHMENT1,
                                GL_COLOR_ATTACHMENT2,
                                GL_COLOR_ATTACHMENT3,
                                GL_COLOR_ATTACHMENT4
                               };
      m_layerTarget->attachSlice(0);
      m_layerTarget->bind();
      glDrawBuffers(5, g_drawBuffers);
      m_layerTarget->clear();

      m_img->bindFullResShader(m_scFullResRaycasterShader, visibleIdxs[0]);
      renderScreenQuad(m_VAO, m_scFullResRaycasterShader);

      m_layerTarget->release();

      // check missed blocks and upload
      std::set<uint32_t> missingBlockIDs;
      std::set<uint32_t> usedBlockIDs;
      tbb::concurrent_unordered_set<uint32_t> ccSet;

      const Z3DTexture* missingBlockIDsTexture = m_layerTarget->attachment(GL_COLOR_ATTACHMENT1);
      if (missingBlockIDsTexture->numPixels() * 4 != m_blockIDs.size()) {
        m_blockIDs.resize(missingBlockIDsTexture->numPixels() * 4);
      }
      size_t numIDs = missingBlockIDsTexture->width() * missingBlockIDsTexture->height() * 4;
      missingBlockIDsTexture->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());

      tbb::parallel_for(
            tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.begin()+numIDs),
            [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range){
        ccSet.insert(range.begin(), range.end()); // inserts a sequence
      }
      );

      if (!ccSet.empty()) {
        missingBlockIDs.insert(ccSet.begin(), ccSet.end());
        ccSet.clear();

        m_layerTarget->attachment(GL_COLOR_ATTACHMENT2)->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());
        tbb::parallel_for(
              tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.begin()+numIDs),
              [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range){
          ccSet.insert(range.begin(), range.end()); // inserts a sequence
        }
        );
        m_layerTarget->attachment(GL_COLOR_ATTACHMENT3)->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());
        tbb::parallel_for(
              tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.begin()+numIDs),
              [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range){
          ccSet.insert(range.begin(), range.end()); // inserts a sequence
        }
        );
        m_layerTarget->attachment(GL_COLOR_ATTACHMENT4)->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());
        tbb::parallel_for(
              tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.begin()+numIDs),
              [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range){
          ccSet.insert(range.begin(), range.end()); // inserts a sequence
        }
        );
        usedBlockIDs.insert(ccSet.begin(), ccSet.end());
      }

      LINFO() << missingBlockIDs.size() << usedBlockIDs.size();
      // render other channels
      size_t i = 0;
      if (missingBlockIDs.empty()) {
        i = 1;
      } else {
        m_img->updateCaches(missingBlockIDs, usedBlockIDs);
      }
      for (; i<visibleIdxs.size(); ++i) {
        m_layerTarget->attachSlice(i);
        m_layerTarget->bind();
        glDrawBuffers(1, g_drawBuffers);
        m_layerTarget->clear();

        m_img->bindFullResShader(m_scFullResRaycasterShader, visibleIdxs[i]);
        renderScreenQuad(m_VAO, m_scFullResRaycasterShader);

        m_layerTarget->release();
      }

      m_scFullResRaycasterShader.release();
    } else {
      m_scRaycasterShader.bind();

      if (!GLVersionGE(3, 0)) {
        m_rendererBase.setGlobalShaderParameters(m_scRaycasterShader, eye);
      }

      float n = m_rendererBase.camera().nearDist();
      float f = m_rendererBase.camera().farDist();
      //http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
      // zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
      float a = f*n/(f-n);
      float b = 0.5f * (f+n)/(f-n) + 0.5f;
      m_scRaycasterShader.setUniform("ze_to_zw_b", b);
      m_scRaycasterShader.setUniform("ze_to_zw_a", a);

      // entry exit points
      m_scRaycasterShader.bindTexture("ray_entry_tex_coord", m_entryTexCoordTexture);
      m_scRaycasterShader.bindTexture("ray_entry_eye_coord", m_entryEyeCoordTexture);
      m_scRaycasterShader.bindTexture("ray_exit_tex_coord",  m_exitTexCoordTexture);
      m_scRaycasterShader.bindTexture("ray_exit_eye_coord", m_exitEyeCoordTexture);

      if (m_compositingMode.get() ==  "ISO Surface")
        m_scRaycasterShader.setUniform("iso_value", m_isoValue.get());

      if (m_compositingMode.get() ==  "Local MIP" || m_compositingMode.get() ==  "Local MIP Opaque")
        m_scRaycasterShader.setUniform("local_MIP_threshold", m_localMIPThreshold.get());

      m_scRaycasterShader.setUniform("sampling_rate", m_samplingRate.get());

      if (visibleIdxs.size() == 1) {
        bindVolumeAndTransferFunc(m_scRaycasterShader, visibleIdxs[0]);
        renderScreenQuad(m_VAO, m_scRaycasterShader);
      } else {
        for (size_t i=0; i<visibleIdxs.size(); ++i) {
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
  }

  if (fullResRendering || visibleIdxs.size() > 1) {
    m_mergeChannelShader.bind();
    m_mergeChannelShader.bindTexture("color_texture", m_layerTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader.bindTexture("depth_texture", m_layerTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(m_VAO, m_mergeChannelShader);
    m_mergeChannelShader.release();
  }

  CHECK_GL_ERROR;
}

void Z3DImgRaycasterRenderer::renderPicking(Z3DEye)
{
}

void Z3DImgRaycasterRenderer::resetTransferFunctions()
{
  for (size_t i=0; i<m_transferFuncParas.size(); ++i) {
    if (m_opaque) {
      m_transferFuncParas[i]->get().reset(
            0.0, 1.0, glm::vec4(0.f),
            glm::vec4(m_img->channelColor(i).r / 255.,
                      m_img->channelColor(i).g / 255.,
                      m_img->channelColor(i).b / 255.,
                      1.f));
      m_transferFuncParas[i]->get().addKey(
            ZColorMapKey(0.001, glm::vec4(0.01f, 0.01f, 0.01f, 0.0f)));
      m_transferFuncParas[i]->get().addKey(
            ZColorMapKey(0.01, glm::vec4(0.01f, 0.01f, 0.01f, 1.0f)));
    } else {
      m_transferFuncParas[i]->get().reset(
            0.0, 1.0, glm::vec4(0.f),
            glm::vec4(m_img->channelColor(i).r / 255.,
                      m_img->channelColor(i).g / 255.,
                      m_img->channelColor(i).b / 255.,
                      1.f));
      //m_transferFuncParas[i]->get().addKey(ZColorMapKey(0.1, glm::vec4(m_volumes[i]->volColor(), 1.f) *
      //                                                  glm::vec4(.1f,.1f,.1f,0.f)));
    }
  }
}

bool Z3DImgRaycasterRenderer::hasVisibleRendering() const
{
  if (m_img) {
    for (size_t i=0; i<m_img->numChannels(); ++i) {
      if (m_channelVisibleParas[i]->get()) {
        return true;
      }
    }
  }
  return false;
}

} // namespace nim

