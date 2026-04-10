#include "z3dsphererenderer.h"

#include "z3dgl.h"
#include "z3dgpuinfo.h"
#include "z3dshaderprogram.h"

#include <gflags/gflags.h>

#include <utility>

namespace nim {

namespace {

constexpr size_t kSphereVerticesPerInstance = 4;
constexpr size_t kSphereIndicesPerInstance = 6;

} // namespace

DEFINE_uint64(atlas_sphere_preferred_instance_budget_per_segment,
              1000000,
              "Preferred sphere instance budget for one renderer-owned segment. This is the normal packing target; "
              "backends still enforce maxMonolithicGeometryStreamBytes as a hard safety guard.");

Z3DSphereRenderer::Z3DSphereRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_dataChanged(false)
  , m_pickingDataChanged(false)
{
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  setUseDisplayList(true);
#endif
  createResources(m_rendererBase.activeBackend());
}

void Z3DSphereRenderer::setData(std::vector<glm::vec4>* pointAndRadiusInput,
                                std::vector<glm::vec4>* specularAndShininessInput)
{
  m_pointAndRadius.clear();
  m_specularAndShininess.clear();
  m_indexs.clear();
  GLuint indices[6] = {0, 1, 2, 2, 1, 3};
  GLuint quadIdx = 0;
  for (auto pr : *pointAndRadiusInput) {
    m_pointAndRadius.push_back(pr);
    m_pointAndRadius.push_back(pr);
    m_pointAndRadius.push_back(pr);
    m_pointAndRadius.push_back(pr);
    for (auto index : indices) {
      m_indexs.push_back(index + 4 * quadIdx);
    }
    quadIdx++;
  }
  if (specularAndShininessInput) {
    for (auto ss : *specularAndShininessInput) {
      m_specularAndShininess.push_back(ss);
      m_specularAndShininess.push_back(ss);
      m_specularAndShininess.push_back(ss);
      m_specularAndShininess.push_back(ss);
    }
  }
  size_t rightUpSize = m_allFlags.size();
  float cornerFlags[4] = {0 << 4 | 0, // (-1, -1) left down
                          2 << 4 | 0, // (1, -1) right down
                          0 << 4 | 2, // (-1, 1) left up
                          2 << 4 | 2}; // (1, 1) right up

  if (rightUpSize > m_pointAndRadius.size()) {
    m_allFlags.resize(m_pointAndRadius.size());
  } else if (rightUpSize < m_pointAndRadius.size()) {
    m_allFlags.resize(m_pointAndRadius.size());
    for (size_t i = rightUpSize; i < m_allFlags.size(); i += 4) {
      m_allFlags[i] = cornerFlags[0];
      m_allFlags[i + 1] = cornerFlags[1];
      m_allFlags[i + 2] = cornerFlags[2];
      m_allFlags[i + 3] = cornerFlags[3];
    }
  }
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
  invalidateOpenglPickingRenderer();
#endif
  m_dataChanged = true;
  m_pickingDataChanged = true;
  // Bump generations for geometry/specular/index/flags on content change
  m_centersGen++;
  m_specularGen++;
  m_flagsGen++;
  m_indexGen++;
}

void Z3DSphereRenderer::setDataColors(std::vector<glm::vec4>* pointColorsInput)
{
  m_pointColors.clear();
  for (auto color : *pointColorsInput) {
    m_pointColors.push_back(color);
    m_pointColors.push_back(color);
    m_pointColors.push_back(color);
    m_pointColors.push_back(color);
  }
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
#endif
  m_dataChanged = true;
  m_colorsGen++;
}

void Z3DSphereRenderer::setDataPickingColors(std::vector<glm::vec4>* pointPickingColorsInput)
{
  m_pointPickingColors.clear();
  if (!pointPickingColorsInput) {
    return;
  }
  for (auto color : *pointPickingColorsInput) {
    m_pointPickingColors.push_back(color);
    m_pointPickingColors.push_back(color);
    m_pointPickingColors.push_back(color);
    m_pointPickingColors.push_back(color);
  }
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglPickingRenderer();
#endif
  m_pickingDataChanged = true;
  m_pickingColorsGen++;
}

void Z3DSphereRenderer::compile()
{
  if (m_rendererBase.activeBackend() != RenderBackend::OpenGL) {
    return;
  }
  DCHECK(m_sphereShaderGrp != nullptr);
  m_dataChanged = true;
  m_sphereShaderGrp->rebuild(m_rendererBase.generateHeader() + generateHeader());
}

std::string Z3DSphereRenderer::generateHeader()
{
  if (!m_useDynamicMaterial) {
    return {};
  }
  return "#define DYNAMIC_MATERIAL_PROPERTY\n";
}

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
void Z3DSphereRenderer::renderUsingOpengl()
{
  if (m_pointAndRadius.empty()) {
    return;
  }
  appendDefaultColors();

  GLUquadricObj* quadric = gluNewQuadric();
  for (size_t i = 0; i < m_pointAndRadius.size(); i += 4) {
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glm::vec3 loc = glm::applyMatrix(coordTransform(), m_pointAndRadius[i].xyz());
    glTranslatef(loc.x, loc.y, loc.z);
    float diameter = m_pointAndRadius[i].w * sizeScale() * 2;
    glScalef(diameter, diameter, diameter);
    // overwrite material property setted by z3drendererbase
    if (m_useDynamicMaterial && !m_specularAndShininess.empty()) {
      glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, m_specularAndShininess[i].w);
      glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, glm::value_ptr(glm::vec4(m_specularAndShininess[i].xyz(), 1.f)));
    }
    glColor4fv(glm::value_ptr(glm::vec4(m_pointColors[i].rgb(), m_pointColors[i].a * opacity())));
    gluSphere(quadric, .5, m_sphereSlicesStacks, m_sphereSlicesStacks);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
  }
  gluDeleteQuadric(quadric);
}

void Z3DSphereRenderer::renderPickingUsingOpengl()
{
  if (m_pointAndRadius.empty()) {
    return;
  }
  if (m_pointPickingColors.empty() || m_pointAndRadius.size() != m_pointPickingColors.size()) {
    return;
  }
  GLUquadricObj* quadric = gluNewQuadric();
  for (size_t i = 0; i < m_pointAndRadius.size(); i += 4) {
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glm::vec3 loc = glm::applyMatrix(coordTransform(), m_pointAndRadius[i].xyz());
    glTranslatef(loc.x, loc.y, loc.z);
    float radius = m_pointAndRadius[i].w * sizeScale();
    glScalef(radius, radius, radius);
    glColor4fv(glm::value_ptr(m_pointPickingColors[i]));
    gluSphere(quadric, 1., 12, 12 /*m_sphereSlicesStacks.get(), m_sphereSlicesStacks.get()*/);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
  }
  gluDeleteQuadric(quadric);
}
#endif

void Z3DSphereRenderer::render(Z3DEye eye)
{
  if (m_pointAndRadius.empty()) {
    return;
  }
  appendDefaultColors();

  m_sphereShaderGrp->bind();
  Z3DShaderProgram& shader = m_sphereShaderGrp->get();

  m_rendererBase.setGlobalShaderParameters(shader, eye);
  setShaderParameters(shader);

  const auto& eyeState = m_rendererBase.viewState().eyes[static_cast<size_t>(eye)];
  float fovy = glm::degrees(eyeState.fieldOfView);
  float adj;
  if (fovy <= 90.f) {
    adj = 1.0027 + 0.000111 * fovy + 0.000098 * fovy * fovy;
  } else {
    adj = 2.02082 - 0.033935 * fovy + 0.00037854 * fovy * fovy;
  }
  shader.setBoxCorrectionUniform(adj);

  const size_t batchVertexBudget =
    std::max<size_t>(1, static_cast<size_t>(FLAGS_atlas_sphere_preferred_instance_budget_per_segment)) *
    kSphereVerticesPerInstance;
  size_t numBatch = std::ceil(m_pointAndRadius.size() * 1.0 / batchVertexBudget);

  if (m_useVAO) {
    if (m_dataChanged) {
      m_VAOs->resize(numBatch);

      m_VBOs.resize(numBatch);
      for (auto& VBO : m_VBOs) {
        if (!VBO) {
          VBO = std::make_unique<Z3DVertexBufferObject>(5);
        } else {
          VBO->resize(5);
        }
      }

      // set vertex data
      auto attr_a_vertex_radius = shader.vertexAttributeLocation();
      GLint attr_a_specular_shininess = -1;
      if (m_useDynamicMaterial && !m_specularAndShininess.empty()) {
        attr_a_specular_shininess = shader.specularShininessAttributeLocation();
      }
      auto attr_color = shader.colorAttributeLocation();
      auto attr_flags = shader.flagsAttributeLocation();

      for (size_t i = 0; i < numBatch; ++i) {
        m_VAOs->bind(i);
        size_t size = batchVertexBudget;
        if (i == numBatch - 1) {
          size = m_pointAndRadius.size() - (numBatch - 1) * batchVertexBudget;
        }
        size_t start = batchVertexBudget * i;

        glEnableVertexAttribArray(attr_a_vertex_radius);
        m_VBOs[i]->bind(GL_ARRAY_BUFFER, 0);
        glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_pointAndRadius[start]), GL_STATIC_DRAW);
        glVertexAttribPointer(attr_a_vertex_radius, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

        if (m_useDynamicMaterial && !m_specularAndShininess.empty()) {
          glEnableVertexAttribArray(attr_a_specular_shininess);
          m_VBOs[i]->bind(GL_ARRAY_BUFFER, 3);
          glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_specularAndShininess[start]), GL_STATIC_DRAW);
          glVertexAttribPointer(attr_a_specular_shininess, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        }

        glEnableVertexAttribArray(attr_color);
        m_VBOs[i]->bind(GL_ARRAY_BUFFER, 1);
        glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_pointColors[start]), GL_STATIC_DRAW);
        glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

        glEnableVertexAttribArray(attr_flags);
        m_VBOs[i]->bind(GL_ARRAY_BUFFER, 2);
        glBufferData(GL_ARRAY_BUFFER, size * sizeof(GLfloat), &(m_allFlags[start]), GL_STATIC_DRAW);
        glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

        m_VBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, 4);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, size * 6 / 4 * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_VAOs->release();
      }

      m_dataChanged = false;
    }

    for (size_t i = 0; i < numBatch; ++i) {
      size_t size = batchVertexBudget;
      if (i == numBatch - 1) {
        size = m_pointAndRadius.size() - (numBatch - 1) * batchVertexBudget;
      }
      m_VAOs->bind(i);
      glDrawElements(GL_TRIANGLES, size * 6 / 4, GL_UNSIGNED_INT, nullptr);
      m_VAOs->release();
    }

  } else {
    if (m_dataChanged) {
      m_VBOs.resize(numBatch);
      for (auto& VBO : m_VBOs) {
        if (!VBO) {
          VBO = std::make_unique<Z3DVertexBufferObject>(5);
        } else {
          VBO->resize(5);
        }
      }
    }
    // set vertex data
    auto attr_a_vertex_radius = shader.vertexAttributeLocation();
    GLint attr_a_specular_shininess = -1;
    if (m_useDynamicMaterial && !m_specularAndShininess.empty()) {
      attr_a_specular_shininess = shader.specularShininessAttributeLocation();
    }
    auto attr_color = shader.colorAttributeLocation();
    auto attr_flags = shader.flagsAttributeLocation();

    for (size_t i = 0; i < numBatch; ++i) {
      size_t size = batchVertexBudget;
      if (i == numBatch - 1) {
        size = m_pointAndRadius.size() - (numBatch - 1) * batchVertexBudget;
      }
      size_t start = batchVertexBudget * i;

      glEnableVertexAttribArray(attr_a_vertex_radius);
      m_VBOs[i]->bind(GL_ARRAY_BUFFER, 0);
      if (m_dataChanged) {
        glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_pointAndRadius[start]), GL_STATIC_DRAW);
      }
      glVertexAttribPointer(attr_a_vertex_radius, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      if (m_useDynamicMaterial && !m_specularAndShininess.empty()) {
        glEnableVertexAttribArray(attr_a_specular_shininess);
        m_VBOs[i]->bind(GL_ARRAY_BUFFER, 3);
        if (m_dataChanged) {
          glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_specularAndShininess[start]), GL_STATIC_DRAW);
        }
        glVertexAttribPointer(attr_a_specular_shininess, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
      }

      glEnableVertexAttribArray(attr_color);
      m_VBOs[i]->bind(GL_ARRAY_BUFFER, 1);
      if (m_dataChanged) {
        glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_pointColors[start]), GL_STATIC_DRAW);
      }
      glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_flags);
      m_VBOs[i]->bind(GL_ARRAY_BUFFER, 2);
      if (m_dataChanged) {
        glBufferData(GL_ARRAY_BUFFER, size * sizeof(GLfloat), &(m_allFlags[start]), GL_STATIC_DRAW);
      }
      glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

      m_VBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, 4);
      if (m_dataChanged) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, size * 6 / 4 * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);
      }

      glDrawElements(GL_TRIANGLES, size * 6 / 4, GL_UNSIGNED_INT, nullptr);

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
      glDisableVertexAttribArray(attr_a_vertex_radius);
      if (m_useDynamicMaterial && !m_specularAndShininess.empty()) {
        glDisableVertexAttribArray(attr_a_specular_shininess);
      }
      glDisableVertexAttribArray(attr_color);
      glDisableVertexAttribArray(attr_flags);
    }

    m_dataChanged = false;
  }

  m_sphereShaderGrp->release();
}

void Z3DSphereRenderer::renderPicking(Z3DEye eye)
{
  if (m_pointAndRadius.empty()) {
    return;
  }

  if (m_pointPickingColors.empty() || m_pointAndRadius.size() != m_pointPickingColors.size()) {
    return;
  }

  m_sphereShaderGrp->bind();
  Z3DShaderProgram& shader = m_sphereShaderGrp->get();

  m_rendererBase.setGlobalShaderParameters(shader, eye);
  setPickingShaderParameters(shader);

  const auto& eyeState = m_rendererBase.viewState().eyes[static_cast<size_t>(eye)];
  float fovy = glm::degrees(eyeState.fieldOfView);
  float adj;
  if (fovy <= 90.f) {
    adj = 1.0027 + 0.000111 * fovy + 0.000098 * fovy * fovy;
  } else {
    adj = 2.02082 - 0.033935 * fovy + 0.00037854 * fovy * fovy;
  }
  shader.setBoxCorrectionUniform(adj);

  const size_t batchVertexBudget =
    std::max<size_t>(1, static_cast<size_t>(FLAGS_atlas_sphere_preferred_instance_budget_per_segment)) *
    kSphereVerticesPerInstance;
  size_t numBatch = std::ceil(m_pointAndRadius.size() * 1.0 / batchVertexBudget);

  if (m_useVAO) {
    if (m_pickingDataChanged) {
      m_pickingVAOs->resize(numBatch);

      m_pickingVBOs.resize(numBatch);
      for (auto& pickingVBO : m_pickingVBOs) {
        if (!pickingVBO) {
          pickingVBO = std::make_unique<Z3DVertexBufferObject>(4);
        } else {
          pickingVBO->resize(4);
        }
      }

      // set vertex data
      auto attr_a_vertex_radius = shader.vertexAttributeLocation();
      auto attr_color = shader.colorAttributeLocation();
      auto attr_flags = shader.flagsAttributeLocation();

      for (size_t i = 0; i < numBatch; ++i) {
        m_pickingVAOs->bind(i);
        size_t size = batchVertexBudget;
        if (i == numBatch - 1) {
          size = m_pointAndRadius.size() - (numBatch - 1) * batchVertexBudget;
        }
        size_t start = batchVertexBudget * i;

        glEnableVertexAttribArray(attr_a_vertex_radius);
        if (m_dataChanged) {
          m_pickingVBOs[i]->bind(GL_ARRAY_BUFFER, 0);
          glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_pointAndRadius[start]), GL_STATIC_DRAW);
        } else {
          m_VBOs[i]->bind(GL_ARRAY_BUFFER, 0);
        }
        glVertexAttribPointer(attr_a_vertex_radius, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

        glEnableVertexAttribArray(attr_color);
        m_pickingVBOs[i]->bind(GL_ARRAY_BUFFER, 1);
        glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_pointPickingColors[start]), GL_STATIC_DRAW);
        glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

        glEnableVertexAttribArray(attr_flags);
        if (m_dataChanged) {
          m_pickingVBOs[i]->bind(GL_ARRAY_BUFFER, 2);
          glBufferData(GL_ARRAY_BUFFER, size * sizeof(GLfloat), &(m_allFlags[start]), GL_STATIC_DRAW);
        } else {
          m_VBOs[i]->bind(GL_ARRAY_BUFFER, 2);
        }
        glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

        if (m_dataChanged) {
          m_pickingVBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, 3);
          glBufferData(GL_ELEMENT_ARRAY_BUFFER, size * 6 / 4 * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);
        } else {
          m_VBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, 4);
        }

        glBindBuffer(GL_ARRAY_BUFFER, 0);

        m_pickingVAOs->release();
      }

      m_pickingDataChanged = false;
    }

    for (size_t i = 0; i < numBatch; ++i) {
      size_t size = batchVertexBudget;
      if (i == numBatch - 1) {
        size = m_pointAndRadius.size() - (numBatch - 1) * batchVertexBudget;
      }
      m_pickingVAOs->bind(i);
      glDrawElements(GL_TRIANGLES, size * 6 / 4, GL_UNSIGNED_INT, nullptr);
      m_pickingVAOs->release();
    }

  } else {
    if (m_pickingDataChanged) {
      m_pickingVBOs.resize(numBatch);
      for (auto& pickingVBO : m_pickingVBOs) {
        if (!pickingVBO) {
          pickingVBO = std::make_unique<Z3DVertexBufferObject>(4);
        } else {
          pickingVBO->resize(4);
        }
      }
    }
    // set vertex data
    auto attr_a_vertex_radius = shader.vertexAttributeLocation();
    auto attr_color = shader.colorAttributeLocation();
    auto attr_flags = shader.flagsAttributeLocation();

    for (size_t i = 0; i < numBatch; ++i) {
      size_t size = batchVertexBudget;
      if (i == numBatch - 1) {
        size = m_pointAndRadius.size() - (numBatch - 1) * batchVertexBudget;
      }
      size_t start = batchVertexBudget * i;

      glEnableVertexAttribArray(attr_a_vertex_radius);
      if (m_dataChanged) {
        m_pickingVBOs[i]->bind(GL_ARRAY_BUFFER, 0);
        if (m_pickingDataChanged) {
          glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_pointAndRadius[start]), GL_STATIC_DRAW);
        }
      } else {
        m_VBOs[i]->bind(GL_ARRAY_BUFFER, 0);
      }
      glVertexAttribPointer(attr_a_vertex_radius, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_color);
      m_pickingVBOs[i]->bind(GL_ARRAY_BUFFER, 1);
      if (m_pickingDataChanged) {
        glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_pointPickingColors[start]), GL_STATIC_DRAW);
      }
      glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_flags);
      if (m_dataChanged) {
        m_pickingVBOs[i]->bind(GL_ARRAY_BUFFER, 2);
        if (m_pickingDataChanged) {
          glBufferData(GL_ARRAY_BUFFER, size * sizeof(GLfloat), &(m_allFlags[start]), GL_STATIC_DRAW);
        }
      } else {
        m_VBOs[i]->bind(GL_ARRAY_BUFFER, 2);
      }
      glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

      if (m_dataChanged) {
        m_pickingVBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, 3);
        if (m_pickingDataChanged) {
          glBufferData(GL_ELEMENT_ARRAY_BUFFER, size * 6 / 4 * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);
        }
      } else {
        m_VBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, 4);
      }

      glDrawElements(GL_TRIANGLES, size * 6 / 4, GL_UNSIGNED_INT, nullptr);

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
      glDisableVertexAttribArray(attr_a_vertex_radius);
      glDisableVertexAttribArray(attr_color);
      glDisableVertexAttribArray(attr_flags);
    }

    m_pickingDataChanged = false;
  }

  m_sphereShaderGrp->release();
}

SpherePayload Z3DSphereRenderer::buildSpherePayload() const
{
  return buildSpherePayload(0, m_pointAndRadius.size() / kSphereVerticesPerInstance, 0u);
}

SpherePayload
Z3DSphereRenderer::buildSpherePayload(size_t instanceStart, size_t instanceCount, uint32_t streamSegmentOrdinal) const
{
  const size_t vertexStart = instanceStart * kSphereVerticesPerInstance;
  const size_t vertexCount = instanceCount * kSphereVerticesPerInstance;
  const size_t indexStart = instanceStart * kSphereIndicesPerInstance;
  const size_t indexCount = instanceCount * kSphereIndicesPerInstance;

  SpherePayload payload;
  payload.streamKey = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
  payload.streamSegmentOrdinal = streamSegmentOrdinal;
  payload.indexValueBias = static_cast<uint32_t>(vertexStart);
  payload.pointsAndRadius = subspanOrEmpty(m_pointAndRadius, vertexStart, vertexCount);
  if (m_pointColors.size() >= vertexStart + vertexCount) {
    payload.colors = subspanOrEmpty(m_pointColors, vertexStart, vertexCount);
  }
  if (m_pointPickingColors.size() >= vertexStart + vertexCount) {
    payload.pickingColors = subspanOrEmpty(m_pointPickingColors, vertexStart, vertexCount);
  }
  if (m_specularAndShininess.size() >= vertexStart + vertexCount) {
    payload.specularAndShininess = subspanOrEmpty(m_specularAndShininess, vertexStart, vertexCount);
  }
  if (m_allFlags.size() >= vertexStart + vertexCount) {
    payload.flags = subspanOrEmpty(m_allFlags, vertexStart, vertexCount);
  }
  if (indexCount > 0) {
    CHECK(indexStart + indexCount <= m_indexs.size()) << "Sphere segment index range exceeds backing storage";
    payload.indices = spanFromGLuints(m_indexs).subspan(indexStart, indexCount);
  }
  payload.useDynamicMaterial = m_useDynamicMaterial;
  payload.wantsLighting = needLighting();
  payload.params = m_rendererBase.parameterState();
  payload.paramsCaptured = true;
  // Per-stream generation counters
  payload.centersGen = m_centersGen;
  payload.colorsGen = m_colorsGen;
  payload.pickingColorsGen = m_pickingColorsGen;
  payload.specularGen = m_specularGen;
  payload.flagsGen = m_flagsGen;
  payload.indexGen = m_indexGen;
  return payload;
}

RenderBatch Z3DSphereRenderer::buildRenderBatch(Z3DEye eye, bool picking) const
{
  return buildRenderBatch(eye, picking, 0, m_pointAndRadius.size() / kSphereVerticesPerInstance, 0u);
}

RenderBatch Z3DSphereRenderer::buildRenderBatch(Z3DEye eye,
                                                bool picking,
                                                size_t instanceStart,
                                                size_t instanceCount,
                                                uint32_t streamSegmentOrdinal) const
{
  RenderBatch batch;

  batch.eye = eye;

  const glm::uvec4 viewport = m_rendererBase.frameState().viewport;
  batch.pass.viewport.origin = glm::vec2(static_cast<float>(viewport.x), static_cast<float>(viewport.y));
  batch.pass.viewport.extent = glm::vec2(static_cast<float>(viewport.z), static_cast<float>(viewport.w));
  batch.pass.viewport.minDepth = 0.f;
  batch.pass.viewport.maxDepth = 1.f;

  const auto& surface = m_rendererBase.frameState().activeSurface;
  batch.pass.colorAttachments = surface.colorAttachments;
  batch.pass.depthAttachment = surface.depthAttachment;

  batch.draw.topology = PrimitiveTopology::TriangleList;

  auto payload = buildSpherePayload(instanceStart, instanceCount, streamSegmentOrdinal);
  payload.pickingPass = picking;
  // GL parity: carry follow flags so Vulkan respects per-renderer toggles
  payload.followCoordTransform = m_followCoordTransform;
  payload.followSizeScale = m_followSizeScale;
  payload.followOpacity = m_followOpacity;
  if (picking) {
    payload.useDynamicMaterial = false;
  }

  batch.draw.vertexCount = static_cast<uint32_t>(payload.pointsAndRadius.size());
  batch.draw.indexCount = static_cast<uint32_t>(payload.indices.size());
  batch.geometry = std::move(payload);
  return batch;
}

void Z3DSphereRenderer::enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking)
{
  if (backend != RenderBackend::Vulkan) {
    return;
  }

  if (m_pointAndRadius.empty()) {
    return;
  }

  if (picking) {
    if (m_pointPickingColors.empty() || m_pointPickingColors.size() != m_pointAndRadius.size()) {
      return;
    }
  }

  appendDefaultColors();
  const auto* renderBackend = m_rendererBase.backend();
  CHECK(renderBackend != nullptr) << "Sphere segmentation requires an active backend";
  const size_t maxStreamBytes = renderBackend->maxMonolithicGeometryStreamBytes();
  const size_t preferredInstanceBudget =
    std::max<size_t>(1, static_cast<size_t>(FLAGS_atlas_sphere_preferred_instance_budget_per_segment));
  const size_t totalInstanceCount = m_pointAndRadius.size() / kSphereVerticesPerInstance;

  auto rangeFitsHardLimit = [&](size_t instanceCount) {
    if (maxStreamBytes == std::numeric_limits<size_t>::max()) {
      return true;
    }
    const size_t vertexCount = instanceCount * kSphereVerticesPerInstance;
    const size_t indexCount = instanceCount * kSphereIndicesPerInstance;
    return vertexCount * sizeof(glm::vec4) <= maxStreamBytes && indexCount * sizeof(uint32_t) <= maxStreamBytes;
  };

  if (totalInstanceCount <= preferredInstanceBudget && rangeFitsHardLimit(totalInstanceCount)) {
    auto batch = buildRenderBatch(eye, picking);
    m_rendererBase.appendBatch(std::move(batch));
    return;
  }

  size_t rangeStart = 0;
  size_t rangeCount = 0;
  uint32_t streamSegmentOrdinal = 0;
  auto flushRange = [&](size_t rangeEnd) {
    if (rangeEnd <= rangeStart) {
      return;
    }
    auto batch = buildRenderBatch(eye, picking, rangeStart, rangeEnd - rangeStart, streamSegmentOrdinal++);
    m_rendererBase.appendBatch(std::move(batch));
    rangeStart = rangeEnd;
    rangeCount = 0;
  };

  for (size_t instanceIndex = 0; instanceIndex < totalInstanceCount; ++instanceIndex) {
    const size_t candidateCount = rangeCount + 1;
    if (instanceIndex > rangeStart &&
        (candidateCount > preferredInstanceBudget || !rangeFitsHardLimit(candidateCount))) {
      flushRange(instanceIndex);
    }
    rangeCount++;
    CHECK(rangeFitsHardLimit(rangeCount))
      << "Renderer-owned sphere segmentation produced a segment that still exceeds the backend hard stream limit";
  }
  flushRange(totalInstanceCount);
}

void Z3DSphereRenderer::appendDefaultColors()
{
  if (m_pointColors.size() < m_pointAndRadius.size()) {
    for (size_t i = m_pointColors.size(); i < m_pointAndRadius.size(); ++i) {
      m_pointColors.emplace_back(0.f, 0.f, 0.f, 1.f);
    }
  }
  if (m_useDynamicMaterial && m_specularAndShininess.size() < m_pointAndRadius.size()) {
    for (size_t i = m_specularAndShininess.size(); i < m_pointAndRadius.size(); ++i) {
      m_specularAndShininess.emplace_back(1.f, 1.f, 1.f, 10.f);
    }
  }
}

void Z3DSphereRenderer::createResources(RenderBackend backend)
{
  if (backend != RenderBackend::OpenGL) {
    return;
  }
  m_sphereShaderGrp = std::make_unique<Z3DShaderGroup>(m_rendererBase);
  QStringList allshaders;
  allshaders << "sphere.vert"
             << "sphere_func.frag"
             << "lighting2.frag";
  m_sphereShaderGrp->init(allshaders, m_rendererBase.generateHeader() + generateHeader());
  m_sphereShaderGrp->addAllSupportedPostShaders();

  m_VAOs = std::make_unique<Z3DVertexArrayObject>(1);
  m_pickingVAOs = std::make_unique<Z3DVertexArrayObject>(1);
  m_VBOs.clear();
  m_pickingVBOs.clear();

  m_dataChanged = true;
  m_pickingDataChanged = true;
}

void Z3DSphereRenderer::destroyResources()
{
  m_sphereShaderGrp.reset();
  m_VAOs.reset();
  m_pickingVAOs.reset();
  m_VBOs.clear();
  m_pickingVBOs.clear();
}

void Z3DSphereRenderer::setUseDynamicMaterial(bool enabled)
{
  if (m_useDynamicMaterial == enabled) {
    return;
  }
  m_useDynamicMaterial = enabled;
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
#endif
  compile();
}

} // namespace nim
