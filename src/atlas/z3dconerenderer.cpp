#include "z3dconerenderer.h"

#include "z3dgl.h"
#include "z3dgpuinfo.h"
#include "z3dshaderprogram.h"

namespace nim {

Z3DConeRenderer::Z3DConeRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_dataChanged(false)
  , m_pickingDataChanged(false)
{
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  setUseDisplayList(true);
#endif
  createResources(m_rendererBase.activeBackend());
}

void Z3DConeRenderer::setData(std::vector<glm::vec4>* baseAndBaseRadius, std::vector<glm::vec4>* axisAndTopRadius)
{
  m_baseAndBaseRadius.clear();
  m_axisAndTopRadius.clear();
  m_allFlags.clear();
  m_indexs.clear();
  if (m_useConeShader2) {
    GLuint indices[6] = {0, 1, 2, 2, 1, 3};
    GLuint quadIdx = 0;
    for (size_t i = 0; i < baseAndBaseRadius->size(); ++i) {
      m_baseAndBaseRadius.push_back((*baseAndBaseRadius)[i]);
      m_baseAndBaseRadius.push_back((*baseAndBaseRadius)[i]);
      m_baseAndBaseRadius.push_back((*baseAndBaseRadius)[i]);
      m_baseAndBaseRadius.push_back((*baseAndBaseRadius)[i]);
      m_axisAndTopRadius.push_back((*axisAndTopRadius)[i]);
      m_axisAndTopRadius.push_back((*axisAndTopRadius)[i]);
      m_axisAndTopRadius.push_back((*axisAndTopRadius)[i]);
      m_axisAndTopRadius.push_back((*axisAndTopRadius)[i]);
      for (auto index : indices) {
        m_indexs.push_back(index + 4 * quadIdx);
      }
      quadIdx++;
    }
    size_t rightUpSize = m_allFlags.size();
    float cornerFlags[4] = {0 << 4 | 0, // (0, 0) left down
                            1 << 4 | 0, // (1, 0) right down
                            0 << 4 | 1, // (0, 1) left up
                            1 << 4 | 1}; // (1, 1) right up

    if (rightUpSize > m_baseAndBaseRadius.size()) {
      m_allFlags.resize(m_baseAndBaseRadius.size());
    } else if (rightUpSize < m_baseAndBaseRadius.size()) {
      m_allFlags.resize(m_baseAndBaseRadius.size());
      for (size_t i = rightUpSize; i < m_allFlags.size(); i += 4) {
        m_allFlags[i] = cornerFlags[0];
        m_allFlags[i + 1] = cornerFlags[1];
        m_allFlags[i + 2] = cornerFlags[2];
        m_allFlags[i + 3] = cornerFlags[3];
      }
    }
  } else {
    GLuint indices[6 * 2 * 3] = {0, 2, 1, 2, 0, 3, 1, 6, 5, 6, 1, 2, 0, 1, 5, 5, 4, 0,
                                 0, 7, 3, 7, 0, 4, 3, 6, 2, 6, 3, 7, 4, 5, 6, 6, 7, 4};
    int32_t rightIdx[8] = {0, 1, 1, 0, 0, 1, 1, 0};
    int32_t upIdx[8] = {0, 0, 1, 1, 0, 0, 1, 1};
    int32_t outIdx[8] = {0, 0, 0, 0, 1, 1, 1, 1};
    GLuint coneIdx = 0;
    for (size_t i = 0; i < baseAndBaseRadius->size(); ++i) {
      for (auto k = 0; k < 8; ++k) {
        m_baseAndBaseRadius.push_back((*baseAndBaseRadius)[i]);
        m_axisAndTopRadius.push_back((*axisAndTopRadius)[i]);
        m_allFlags.push_back(rightIdx[k] << 8 | upIdx[k] << 4 | outIdx[k]);
      }
      for (auto index : indices) {
        m_indexs.push_back(index + 8 * coneIdx);
      }
      coneIdx++;
    }
  }

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
  invalidateOpenglPickingRenderer();
#endif
  m_dataChanged = true;
  m_pickingDataChanged = true;
}

void Z3DConeRenderer::setDataColors(std::vector<glm::vec4>* coneColors)
{
  m_coneBaseColors.clear();
  m_coneTopColors.clear();
  m_sameColorForBaseAndTop = true;
  auto dup = m_useConeShader2 ? 4 : 8;
  for (auto color : *coneColors) {
    for (auto k = 0; k < dup; ++k) {
      m_coneBaseColors.push_back(color);
    }
  }
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
#endif
  m_dataChanged = true;
}

void Z3DConeRenderer::setDataColors(std::vector<glm::vec4>* coneBaseColors, std::vector<glm::vec4>* coneTopColors)
{
  m_coneBaseColors.clear();
  m_coneTopColors.clear();
  m_sameColorForBaseAndTop = false;
  auto dup = m_useConeShader2 ? 4 : 8;
  for (size_t i = 0; i < coneBaseColors->size(); ++i) {
    for (auto k = 0; k < dup; ++k) {
      m_coneBaseColors.push_back((*coneBaseColors)[i]);
      m_coneTopColors.push_back((*coneTopColors)[i]);
    }
  }
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
#endif
  m_dataChanged = true;
}

void Z3DConeRenderer::setDataPickingColors(std::vector<glm::vec4>* conePickingColors)
{
  m_conePickingColors.clear();
  if (!conePickingColors) {
    return;
  }
  auto dup = m_useConeShader2 ? 4 : 8;
  for (auto color : *conePickingColors) {
    for (auto k = 0; k < dup; ++k) {
      m_conePickingColors.push_back(color);
    }
  }
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglPickingRenderer();
#endif
  m_pickingDataChanged = true;
}

void Z3DConeRenderer::compile()
{
  m_coneShaderGrp->rebuild(m_rendererBase.generateHeader() + generateHeader());
}

std::string Z3DConeRenderer::generateHeader()
{
  const char* define = "FLAT_CAPS";
  switch (m_coneCapStyle) {
    case ConeCapStyle::FlatCaps:
      define = "FLAT_CAPS";
      break;
    case ConeCapStyle::NoCaps:
      define = "NO_CAPS";
      break;
    case ConeCapStyle::RoundCaps:
      define = "ROUND_CAPS";
      break;
    case ConeCapStyle::RoundBaseFlatTop:
      define = "ROUND_BASE_CAP_FLAT_TOP_CAP";
      break;
    case ConeCapStyle::FlatBaseRoundTop:
      define = "FLAT_BASE_CAP_ROUND_TOP_CAP";
      break;
  }

  return fmt::format("#define {}\n", define);
}

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
void Z3DConeRenderer::renderUsingOpengl()
{
  if (m_baseAndBaseRadius.empty()) {
    return;
  }
  appendDefaultColors();

  GLUquadricObj* quadric = gluNewQuadric();
  auto dup = m_useConeShader2 ? 4 : 8;
  for (size_t i = 0; i < m_baseAndBaseRadius.size(); i += dup) {
    glColor4fv(glm::value_ptr(glm::vec4(m_coneBaseColors[i].rgb(), m_coneBaseColors[i].a * opacity())));
    glm::vec3 bottomPos = m_baseAndBaseRadius[i].xyz();
    glm::vec3 topPos = m_axisAndTopRadius[i].xyz();
    topPos += bottomPos;
    // bottomPos *= getCoordTransform();
    bottomPos = glm::applyMatrix(coordTransform(), bottomPos);
    // topPos *= getCoordTransform();
    topPos = glm::applyMatrix(coordTransform(), topPos);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glm::vec3 C = topPos - bottomPos;
    float height = glm::length(C);
    glm::vec3 A, B;
    C = glm::normalize(C);
    glm::getOrthogonalVectors(C, A, B);
    glm::mat4 m(glm::vec4(A, 0.f), glm::vec4(B, 0.f), glm::vec4(C, 0.f), glm::vec4(bottomPos, 1.f));
    glMultMatrixf(&m[0][0]);

    gluCylinder(quadric,
                sizeScale() * m_baseAndBaseRadius[i].w,
                sizeScale() * m_axisAndTopRadius[i].w,
                height,
                m_cylinderSubdivisionAroundZ,
                m_cylinderSubdivisionAlongZ);

    if (m_coneCapStyle == ConeCapStyle::RoundCaps || m_coneCapStyle == ConeCapStyle::RoundBaseFlatTop) {
      gluSphere(quadric,
                sizeScale() * m_baseAndBaseRadius[i].w,
                m_cylinderSubdivisionAroundZ,
                m_cylinderSubdivisionAroundZ);
    } else if (m_coneCapStyle == ConeCapStyle::FlatCaps || m_coneCapStyle == ConeCapStyle::FlatBaseRoundTop) {
      gluQuadricOrientation(quadric, GLU_INSIDE);
      gluDisk(quadric, 0.0, sizeScale() * m_baseAndBaseRadius[i].w, m_cylinderSubdivisionAroundZ, 1);
      gluQuadricOrientation(quadric, GLU_OUTSIDE);
    }

    if (m_coneCapStyle == ConeCapStyle::RoundCaps || m_coneCapStyle == ConeCapStyle::FlatBaseRoundTop) {
      glTranslatef(0, 0, height);
      if (!m_sameColorForBaseAndTop) {
        glColor4fv(glm::value_ptr(glm::vec4(m_coneTopColors[i].rgb(), m_coneTopColors[i].a * opacity())));
      }
      gluSphere(quadric,
                sizeScale() * m_axisAndTopRadius[i].w,
                m_cylinderSubdivisionAroundZ,
                m_cylinderSubdivisionAroundZ);
    } else if (m_coneCapStyle == ConeCapStyle::FlatCaps || m_coneCapStyle == ConeCapStyle::RoundBaseFlatTop) {
      glTranslatef(0, 0, height);
      gluDisk(quadric, 0.0, sizeScale() * m_axisAndTopRadius[i].w, m_cylinderSubdivisionAroundZ, 1);
    }

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
  }
  gluDeleteQuadric(quadric);
}

void Z3DConeRenderer::renderPickingUsingOpengl()
{
  if (m_baseAndBaseRadius.empty()) {
    return;
  }
  if (m_conePickingColors.empty() || m_conePickingColors.size() != m_baseAndBaseRadius.size()) {
    return;
  }

  GLUquadricObj* quadric = gluNewQuadric();
  auto dup = m_useConeShader2 ? 4 : 8;
  for (size_t i = 0; i < m_baseAndBaseRadius.size(); i += dup) {
    glColor4fv(glm::value_ptr(m_conePickingColors[i]));
    glm::vec3 bottomPos = m_baseAndBaseRadius[i].xyz();
    glm::vec3 topPos = m_axisAndTopRadius[i].xyz();
    topPos += bottomPos;
    // bottomPos *= getCoordTransform();
    bottomPos = glm::applyMatrix(coordTransform(), bottomPos);
    // topPos *= getCoordTransform();
    topPos = glm::applyMatrix(coordTransform(), topPos);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glm::vec3 C = topPos - bottomPos;
    float height = glm::length(C);
    glm::vec3 A, B;
    C = glm::normalize(C);
    glm::getOrthogonalVectors(C, A, B);
    glm::mat4 m(glm::vec4(A, 0.f), glm::vec4(B, 0.f), glm::vec4(C, 0.f), glm::vec4(bottomPos, 1.f));
    glMultMatrixf(&m[0][0]);

    gluCylinder(quadric,
                sizeScale() * m_baseAndBaseRadius[i].w,
                sizeScale() * m_axisAndTopRadius[i].w,
                height,
                m_cylinderSubdivisionAroundZ,
                m_cylinderSubdivisionAlongZ);

    if (m_coneCapStyle == ConeCapStyle::RoundCaps || m_coneCapStyle == ConeCapStyle::RoundBaseFlatTop) {
      gluSphere(quadric, sizeScale() * m_baseAndBaseRadius[i].w, 12, 12);
    } else if (m_coneCapStyle == ConeCapStyle::FlatCaps || m_coneCapStyle == ConeCapStyle::FlatBaseRoundTop) {
      gluQuadricOrientation(quadric, GLU_INSIDE);
      gluDisk(quadric, 0.0, sizeScale() * m_baseAndBaseRadius[i].w, 12, 1);
      gluQuadricOrientation(quadric, GLU_OUTSIDE);
    }

    if (m_coneCapStyle == ConeCapStyle::RoundCaps || m_coneCapStyle == ConeCapStyle::FlatBaseRoundTop) {
      glTranslatef(0, 0, height);
      gluSphere(quadric, sizeScale() * m_axisAndTopRadius[i].w, 12, 12);
    } else if (m_coneCapStyle == ConeCapStyle::FlatCaps || m_coneCapStyle == ConeCapStyle::RoundBaseFlatTop) {
      glTranslatef(0, 0, height);
      gluDisk(quadric, 0.0, sizeScale() * m_axisAndTopRadius[i].w, 12, 1);
    }

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
  }
  gluDeleteQuadric(quadric);
}
#endif

void Z3DConeRenderer::render(Z3DEye eye)
{
  if (m_baseAndBaseRadius.empty()) {
    return;
  }

  appendDefaultColors();

  m_coneShaderGrp->bind();
  Z3DShaderProgram& shader = m_coneShaderGrp->get();

  m_rendererBase.setGlobalShaderParameters(shader, eye);
  setShaderParameters(shader);

  if (m_useVAO) {
    if (m_dataChanged) {
      m_VAO->bind();
      // set vertex data
      auto attr_origin = shader.originAttributeLocation();
      auto attr_axis = shader.axisAttributeLocation();
      auto attr_flags = shader.flagsAttributeLocation();
      auto attr_colors = shader.colorAttributeLocation();
      auto attr_colors2 = shader.color2AttributeLocation();

      glEnableVertexAttribArray(attr_origin);
      m_VBOs->bind(GL_ARRAY_BUFFER, 0);
      glBufferData(GL_ARRAY_BUFFER,
                   m_baseAndBaseRadius.size() * 4 * sizeof(GLfloat),
                   m_baseAndBaseRadius.data(),
                   GL_STATIC_DRAW);
      glVertexAttribPointer(attr_origin, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_axis);
      m_VBOs->bind(GL_ARRAY_BUFFER, 1);
      glBufferData(GL_ARRAY_BUFFER,
                   m_axisAndTopRadius.size() * 4 * sizeof(GLfloat),
                   m_axisAndTopRadius.data(),
                   GL_STATIC_DRAW);
      glVertexAttribPointer(attr_axis, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_flags);
      m_VBOs->bind(GL_ARRAY_BUFFER, 2);
      glBufferData(GL_ARRAY_BUFFER, m_allFlags.size() * sizeof(GLfloat), m_allFlags.data(), GL_STATIC_DRAW);
      glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_colors);
      m_VBOs->bind(GL_ARRAY_BUFFER, 3);
      glBufferData(GL_ARRAY_BUFFER,
                   m_coneBaseColors.size() * 4 * sizeof(GLfloat),
                   m_coneBaseColors.data(),
                   GL_STATIC_DRAW);
      glVertexAttribPointer(attr_colors, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      if (m_sameColorForBaseAndTop) {
        glEnableVertexAttribArray(attr_colors2);
        glVertexAttribPointer(attr_colors2, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
      } else {
        glEnableVertexAttribArray(attr_colors2);
        m_VBOs->bind(GL_ARRAY_BUFFER, 4);
        glBufferData(GL_ARRAY_BUFFER,
                     m_coneTopColors.size() * 4 * sizeof(GLfloat),
                     m_coneTopColors.data(),
                     GL_STATIC_DRAW);
        glVertexAttribPointer(attr_colors2, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
      }

      m_VBOs->bind(GL_ELEMENT_ARRAY_BUFFER, 5);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indexs.size() * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      m_VAO->release();

      m_dataChanged = false;
    }

    m_VAO->bind();
    glDrawElements(GL_TRIANGLES, m_indexs.size(), GL_UNSIGNED_INT, nullptr);
    m_VAO->release();

  } else {
    // set vertex data
    auto attr_origin = shader.originAttributeLocation();
    auto attr_axis = shader.axisAttributeLocation();
    auto attr_flags = shader.flagsAttributeLocation();
    auto attr_colors = shader.colorAttributeLocation();
    auto attr_colors2 = shader.color2AttributeLocation();

    glEnableVertexAttribArray(attr_origin);
    m_VBOs->bind(GL_ARRAY_BUFFER, 0);
    if (m_dataChanged) {
      glBufferData(GL_ARRAY_BUFFER,
                   m_baseAndBaseRadius.size() * 4 * sizeof(GLfloat),
                   m_baseAndBaseRadius.data(),
                   GL_STATIC_DRAW);
    }
    glVertexAttribPointer(attr_origin, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_axis);
    m_VBOs->bind(GL_ARRAY_BUFFER, 1);
    if (m_dataChanged) {
      glBufferData(GL_ARRAY_BUFFER,
                   m_axisAndTopRadius.size() * 4 * sizeof(GLfloat),
                   m_axisAndTopRadius.data(),
                   GL_STATIC_DRAW);
    }
    glVertexAttribPointer(attr_axis, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_flags);
    m_VBOs->bind(GL_ARRAY_BUFFER, 2);
    if (m_dataChanged) {
      glBufferData(GL_ARRAY_BUFFER, m_allFlags.size() * sizeof(GLfloat), m_allFlags.data(), GL_STATIC_DRAW);
    }
    glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_colors);
    m_VBOs->bind(GL_ARRAY_BUFFER, 3);
    if (m_dataChanged) {
      glBufferData(GL_ARRAY_BUFFER,
                   m_coneBaseColors.size() * 4 * sizeof(GLfloat),
                   m_coneBaseColors.data(),
                   GL_STATIC_DRAW);
    }
    glVertexAttribPointer(attr_colors, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    if (m_sameColorForBaseAndTop) {
      glEnableVertexAttribArray(attr_colors2);
      glVertexAttribPointer(attr_colors2, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    } else {
      glEnableVertexAttribArray(attr_colors2);
      m_VBOs->bind(GL_ARRAY_BUFFER, 4);
      if (m_dataChanged) {
        glBufferData(GL_ARRAY_BUFFER,
                     m_coneTopColors.size() * 4 * sizeof(GLfloat),
                     m_coneTopColors.data(),
                     GL_STATIC_DRAW);
      }
      glVertexAttribPointer(attr_colors2, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    m_VBOs->bind(GL_ELEMENT_ARRAY_BUFFER, 5);
    if (m_dataChanged) {
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indexs.size() * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);
    }

    glDrawElements(GL_TRIANGLES, m_indexs.size(), GL_UNSIGNED_INT, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    glDisableVertexAttribArray(attr_origin);
    glDisableVertexAttribArray(attr_axis);
    glDisableVertexAttribArray(attr_flags);
    glDisableVertexAttribArray(attr_colors);
    glDisableVertexAttribArray(attr_colors2);

    m_dataChanged = false;
  }

  m_coneShaderGrp->release();
}

void Z3DConeRenderer::renderPicking(Z3DEye eye)
{
  if (m_baseAndBaseRadius.empty()) {
    return;
  }

  if (m_conePickingColors.empty() || m_conePickingColors.size() != m_baseAndBaseRadius.size()) {
    return;
  }

  m_coneShaderGrp->bind();
  Z3DShaderProgram& shader = m_coneShaderGrp->get();

  m_rendererBase.setGlobalShaderParameters(shader, eye);
  setPickingShaderParameters(shader);

  if (m_useVAO) {
    if (m_pickingDataChanged) {
      m_pickingVAO->bind();
      // set vertex data
      auto attr_origin = shader.originAttributeLocation();
      auto attr_axis = shader.axisAttributeLocation();
      auto attr_flags = shader.flagsAttributeLocation();
      auto attr_colors = shader.colorAttributeLocation();
      auto attr_colors2 = shader.color2AttributeLocation();

      glEnableVertexAttribArray(attr_origin);
      if (m_dataChanged) {
        m_pickingVBOs->bind(GL_ARRAY_BUFFER, 0);
        glBufferData(GL_ARRAY_BUFFER,
                     m_baseAndBaseRadius.size() * 4 * sizeof(GLfloat),
                     m_baseAndBaseRadius.data(),
                     GL_STATIC_DRAW);
      } else {
        m_VBOs->bind(GL_ARRAY_BUFFER, 0);
      }
      glVertexAttribPointer(attr_origin, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_axis);
      if (m_dataChanged) {
        m_pickingVBOs->bind(GL_ARRAY_BUFFER, 1);
        glBufferData(GL_ARRAY_BUFFER,
                     m_axisAndTopRadius.size() * 4 * sizeof(GLfloat),
                     m_axisAndTopRadius.data(),
                     GL_STATIC_DRAW);
      } else {
        m_VBOs->bind(GL_ARRAY_BUFFER, 1);
      }
      glVertexAttribPointer(attr_axis, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_flags);
      if (m_dataChanged) {
        m_pickingVBOs->bind(GL_ARRAY_BUFFER, 2);
        glBufferData(GL_ARRAY_BUFFER, m_allFlags.size() * sizeof(GLfloat), m_allFlags.data(), GL_STATIC_DRAW);
      } else {
        m_VBOs->bind(GL_ARRAY_BUFFER, 2);
      }
      glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_colors);
      m_pickingVBOs->bind(GL_ARRAY_BUFFER, 3);
      glBufferData(GL_ARRAY_BUFFER,
                   m_conePickingColors.size() * 4 * sizeof(GLfloat),
                   m_conePickingColors.data(),
                   GL_STATIC_DRAW);
      glVertexAttribPointer(attr_colors, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_colors2);
      glVertexAttribPointer(attr_colors2, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      if (m_dataChanged) {
        m_pickingVBOs->bind(GL_ELEMENT_ARRAY_BUFFER, 4);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indexs.size() * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);
      } else {
        m_VBOs->bind(GL_ELEMENT_ARRAY_BUFFER, 5);
      }

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      m_pickingVAO->release();

      m_pickingDataChanged = false;
    }

    m_pickingVAO->bind();
    glDrawElements(GL_TRIANGLES, m_indexs.size(), GL_UNSIGNED_INT, nullptr);
    m_pickingVAO->release();

  } else {
    // set vertex data
    auto attr_origin = shader.originAttributeLocation();
    auto attr_axis = shader.axisAttributeLocation();
    auto attr_flags = shader.flagsAttributeLocation();
    auto attr_colors = shader.colorAttributeLocation();
    auto attr_colors2 = shader.color2AttributeLocation();

    glEnableVertexAttribArray(attr_origin);
    if (m_dataChanged) {
      m_pickingVBOs->bind(GL_ARRAY_BUFFER, 0);
      if (m_pickingDataChanged) {
        glBufferData(GL_ARRAY_BUFFER,
                     m_baseAndBaseRadius.size() * 4 * sizeof(GLfloat),
                     m_baseAndBaseRadius.data(),
                     GL_STATIC_DRAW);
      }
    } else {
      m_VBOs->bind(GL_ARRAY_BUFFER, 0);
    }
    glVertexAttribPointer(attr_origin, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_axis);
    if (m_dataChanged) {
      m_pickingVBOs->bind(GL_ARRAY_BUFFER, 1);
      if (m_pickingDataChanged) {
        glBufferData(GL_ARRAY_BUFFER,
                     m_axisAndTopRadius.size() * 4 * sizeof(GLfloat),
                     m_axisAndTopRadius.data(),
                     GL_STATIC_DRAW);
      }
    } else {
      m_VBOs->bind(GL_ARRAY_BUFFER, 1);
    }
    glVertexAttribPointer(attr_axis, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_flags);
    if (m_dataChanged) {
      m_pickingVBOs->bind(GL_ARRAY_BUFFER, 2);
      if (m_pickingDataChanged) {
        glBufferData(GL_ARRAY_BUFFER, m_allFlags.size() * sizeof(GLfloat), m_allFlags.data(), GL_STATIC_DRAW);
      }
    } else {
      m_VBOs->bind(GL_ARRAY_BUFFER, 2);
    }
    glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_colors);
    m_pickingVBOs->bind(GL_ARRAY_BUFFER, 3);
    if (m_pickingDataChanged) {
      glBufferData(GL_ARRAY_BUFFER,
                   m_conePickingColors.size() * 4 * sizeof(GLfloat),
                   m_conePickingColors.data(),
                   GL_STATIC_DRAW);
    }
    glVertexAttribPointer(attr_colors, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_colors2);
    glVertexAttribPointer(attr_colors2, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    if (m_dataChanged) {
      m_pickingVBOs->bind(GL_ELEMENT_ARRAY_BUFFER, 4);
      if (m_pickingDataChanged) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indexs.size() * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);
      }
    } else {
      m_VBOs->bind(GL_ELEMENT_ARRAY_BUFFER, 5);
    }

    glDrawElements(GL_TRIANGLES, m_indexs.size(), GL_UNSIGNED_INT, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    glDisableVertexAttribArray(attr_origin);
    glDisableVertexAttribArray(attr_axis);
    glDisableVertexAttribArray(attr_flags);
    glDisableVertexAttribArray(attr_colors);
    glDisableVertexAttribArray(attr_colors2);

    m_pickingDataChanged = false;
  }

  m_coneShaderGrp->release();
}

void Z3DConeRenderer::appendDefaultColors()
{
  if (m_coneBaseColors.size() < m_baseAndBaseRadius.size()) {
    if (m_sameColorForBaseAndTop) {
      for (size_t i = m_coneBaseColors.size(); i < m_baseAndBaseRadius.size(); ++i) {
        m_coneBaseColors.emplace_back(0.f, 0.f, 0.f, 1.f);
      }
    } else {
      for (size_t i = m_coneBaseColors.size(); i < m_baseAndBaseRadius.size(); ++i) {
        m_coneBaseColors.emplace_back(0.f, 0.f, 0.f, 1.f);
        m_coneTopColors.emplace_back(0.f, 0.f, 0.f, 1.f);
      }
    }
  }
}

void Z3DConeRenderer::enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking)
{
  if (backend != RenderBackend::Vulkan) {
    return;
  }

  if (m_baseAndBaseRadius.empty()) {
    return;
  }

  if (picking && (m_conePickingColors.size() < m_baseAndBaseRadius.size())) {
    return;
  }

  appendDefaultColors();

  auto batch = buildRenderBatch(eye, picking);
  m_rendererBase.appendBatch(std::move(batch));
}

ConePayload Z3DConeRenderer::buildConePayload() const
{
  ConePayload payload;

  payload.renderer = const_cast<Z3DConeRenderer*>(this);
  payload.baseAndRadius = spanOrEmpty(m_baseAndBaseRadius);
  payload.axisAndTopRadius = spanOrEmpty(m_axisAndTopRadius);
  payload.baseColors = spanOrEmpty(m_coneBaseColors);
  payload.topColors = spanOrEmpty(m_coneTopColors);
  payload.pickingColors = spanOrEmpty(m_conePickingColors);
  payload.flags = spanFromGLfloats(m_allFlags);
  payload.indices = spanFromGLuints(m_indexs);

  switch (m_coneCapStyle) {
    case ConeCapStyle::FlatCaps:
      payload.capStyle = ConePayload::CapStyle::FlatCaps;
      break;
    case ConeCapStyle::NoCaps:
      payload.capStyle = ConePayload::CapStyle::NoCaps;
      break;
    case ConeCapStyle::RoundCaps:
      payload.capStyle = ConePayload::CapStyle::RoundCaps;
      break;
    case ConeCapStyle::RoundBaseFlatTop:
      payload.capStyle = ConePayload::CapStyle::RoundBaseFlatTop;
      break;
    case ConeCapStyle::FlatBaseRoundTop:
      payload.capStyle = ConePayload::CapStyle::FlatBaseRoundTop;
      break;
  }

  payload.subdivisionAround = m_cylinderSubdivisionAroundZ;
  payload.subdivisionAlong = m_cylinderSubdivisionAlongZ;
  payload.sameColorForBaseAndTop = m_sameColorForBaseAndTop;
  payload.useConeShader2 = m_useConeShader2;
  return payload;
}

RenderBatch Z3DConeRenderer::buildRenderBatch(Z3DEye eye, bool picking) const
{
  RenderBatch batch;

  batch.eye = eye;

  const glm::uvec4 viewport = m_rendererBase.frameState().viewport;
  batch.pass.extent = glm::uvec2(viewport.z, viewport.w);
  batch.pass.viewport.origin = glm::vec2(static_cast<float>(viewport.x), static_cast<float>(viewport.y));
  batch.pass.viewport.extent = glm::vec2(static_cast<float>(viewport.z), static_cast<float>(viewport.w));
  batch.pass.viewport.minDepth = 0.f;
  batch.pass.viewport.maxDepth = 1.f;

  const auto& surface = m_rendererBase.frameState().activeSurface;
  batch.pass.colorAttachments = surface.colorAttachments;
  batch.pass.depthAttachment = surface.depthAttachment;

  batch.draw.topology = PrimitiveTopology::TriangleList;
  batch.draw.vertexCount = static_cast<uint32_t>(m_baseAndBaseRadius.size());
  batch.draw.indexCount = static_cast<uint32_t>(m_indexs.size());
  auto payload = buildConePayload();
  payload.pickingPass = picking;
  batch.geometry = std::move(payload);
  return batch;
}

void Z3DConeRenderer::createResources(RenderBackend backend)
{
  if (backend != RenderBackend::OpenGL) {
    return;
  }
  m_coneShaderGrp = std::make_unique<Z3DShaderGroup>(m_rendererBase);
  QStringList allshaders;
  if (m_useConeShader2) {
    allshaders << "cone_2.vert"
               << "cone_func_2.frag"
               << "lighting2.frag";
  } else {
    allshaders << "cone.vert"
               << "cone_func.frag"
               << "lighting2.frag";
  }
  m_coneShaderGrp->init(allshaders, m_rendererBase.generateHeader() + generateHeader());
  m_coneShaderGrp->addAllSupportedPostShaders();

  m_VAO = std::make_unique<Z3DVertexArrayObject>(1);
  m_pickingVAO = std::make_unique<Z3DVertexArrayObject>(1);
  m_VBOs = std::make_unique<Z3DVertexBufferObject>(6);
  m_pickingVBOs = std::make_unique<Z3DVertexBufferObject>(5);

  m_dataChanged = true;
  m_pickingDataChanged = true;
}

void Z3DConeRenderer::destroyResources()
{
  m_coneShaderGrp.reset();
  m_VAO.reset();
  m_pickingVAO.reset();
  m_VBOs.reset();
  m_pickingVBOs.reset();
}

void Z3DConeRenderer::setConeCapStyle(ConeCapStyle style)
{
  if (m_coneCapStyle == style) {
    return;
  }
  m_coneCapStyle = style;
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
  invalidateOpenglPickingRenderer();
#endif
  if (m_rendererBase.activeBackend() == RenderBackend::OpenGL) {
    compile();
  }
}

void Z3DConeRenderer::setCylinderSubdivisionAroundZ(int subdivisions)
{
  int clamped = std::max(1, subdivisions);
  if (m_cylinderSubdivisionAroundZ == clamped) {
    return;
  }
  m_cylinderSubdivisionAroundZ = clamped;
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
#endif
}

void Z3DConeRenderer::setCylinderSubdivisionAlongZ(int subdivisions)
{
  int clamped = std::max(1, subdivisions);
  if (m_cylinderSubdivisionAlongZ == clamped) {
    return;
  }
  m_cylinderSubdivisionAlongZ = clamped;
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
#endif
}

} // namespace nim
