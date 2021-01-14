#include "z3dellipsoidrenderer.h"

#include "z3dgl.h"
#include "z3dgpuinfo.h"
#include "z3dshaderprogram.h"

namespace nim {

Z3DEllipsoidRenderer::Z3DEllipsoidRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_ellipsoidShaderGrp(rendererBase)
  , m_sphereSlicesStacks("Sphere Slices Stacks", 36, 20, 100)
  , m_useDynamicMaterial("Calculate Material Property From Intensity", true)
  , m_VAO(1)
  , m_pickingVAO(1)
  , m_VBOs(8)
  , m_pickingVBOs(7)
  , m_dataChanged(false)
  , m_pickingDataChanged(false)
{
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  setUseDisplayList(true);
  connect(&m_sphereSlicesStacks, &ZIntParameter::valueChanged, this, &Z3DEllipsoidRenderer::invalidateOpenglRenderer);
  connect(&m_useDynamicMaterial, &ZBoolParameter::valueChanged, this, &Z3DEllipsoidRenderer::invalidateOpenglRenderer);
#endif

  connect(&m_useDynamicMaterial, &ZBoolParameter::valueChanged, this, &Z3DEllipsoidRenderer::compile);

  QStringList allshaders;
  allshaders << "ellipsoid.vert" << "ellipsoid_func.frag" << "lighting2.frag";
  m_ellipsoidShaderGrp.init(allshaders, m_rendererBase.generateHeader() + generateHeader());
  m_ellipsoidShaderGrp.addAllSupportedPostShaders();
  CHECK_GL_ERROR
}

void Z3DEllipsoidRenderer::setData(std::vector<glm::vec3>* centers, std::vector<glm::vec3>* axis1,
                                   std::vector<glm::vec3>* axis2, std::vector<glm::vec3>* axis3,
                                   std::vector<glm::vec4>* specularAndShininessInput)
{
  m_centers.clear();
  m_axis1.clear();
  m_axis2.clear();
  m_axis3.clear();
  m_specularAndShininess.clear();
  m_indexs.clear();
  GLuint indices[6] = {0, 1, 2, 2, 1, 3};
  GLuint quadIdx = 0;
  for (size_t i = 0; i < centers->size(); ++i) {
    glm::mat4 T(glm::vec4((* axis1)[i],
    0.f),
    glm::vec4((*axis2)[i], 0.f),
      glm::vec4((*axis3)[i], 0.f),
      glm::vec4((*centers)[i], 1.f));
    m_centers.push_back(T[3]);
    m_centers.push_back(T[3]);
    m_centers.push_back(T[3]);
    m_centers.push_back(T[3]);
    m_axis1.push_back(T[0]);
    m_axis1.push_back(T[0]);
    m_axis1.push_back(T[0]);
    m_axis1.push_back(T[0]);
    m_axis2.push_back(T[1]);
    m_axis2.push_back(T[1]);
    m_axis2.push_back(T[1]);
    m_axis2.push_back(T[1]);
    m_axis3.push_back(T[2]);
    m_axis3.push_back(T[2]);
    m_axis3.push_back(T[2]);
    m_axis3.push_back(T[2]);
    for (auto index : indices) {
      m_indexs.push_back(index + 4 * quadIdx);
    }
    quadIdx++;
  }
  if (!specularAndShininessInput) {
    m_useDynamicMaterial.set(false);
  } else {
    for (auto ss : *specularAndShininessInput) {
      m_specularAndShininess.push_back(ss);
      m_specularAndShininess.push_back(ss);
      m_specularAndShininess.push_back(ss);
      m_specularAndShininess.push_back(ss);
    }
  }
  size_t rightUpSize = m_allFlags.size();
  float cornerFlags[4] = {0 << 4 | 0,      // (-1, -1) left down
                          2 << 4 | 0,      // (1, -1) right down
                          0 << 4 | 2,      // (-1, 1) left up
                          2 << 4 | 2};     // (1, 1) right up

  if (rightUpSize > m_centers.size()) {
    m_allFlags.resize(m_centers.size());
  } else if (rightUpSize < m_centers.size()) {
    m_allFlags.resize(m_centers.size());
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
}

void Z3DEllipsoidRenderer::setDataColors(std::vector<glm::vec4>* ellipsoidColorsInput)
{
  m_ellipsoidColors.clear();
  for (auto color : *ellipsoidColorsInput) {
    m_ellipsoidColors.push_back(color);
    m_ellipsoidColors.push_back(color);
    m_ellipsoidColors.push_back(color);
    m_ellipsoidColors.push_back(color);
  }
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
#endif
  m_dataChanged = true;
}

void Z3DEllipsoidRenderer::setDataPickingColors(std::vector<glm::vec4>* ellipsoidPickingColorsInput)
{
  m_ellipsoidPickingColors.clear();
  if (!ellipsoidPickingColorsInput) {
    return;
  }
  for (auto color : *ellipsoidPickingColorsInput) {
    m_ellipsoidPickingColors.push_back(color);
    m_ellipsoidPickingColors.push_back(color);
    m_ellipsoidPickingColors.push_back(color);
    m_ellipsoidPickingColors.push_back(color);
  }
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglPickingRenderer();
#endif
  m_pickingDataChanged = true;
}

void Z3DEllipsoidRenderer::compile()
{
  m_dataChanged = true;
  m_ellipsoidShaderGrp.rebuild(m_rendererBase.generateHeader() + generateHeader());
}

QString Z3DEllipsoidRenderer::generateHeader()
{
  QString headerSource;
  if (m_useDynamicMaterial.get()) {
    headerSource += "#define DYNAMIC_MATERIAL_PROPERTY\n";
  }
  return headerSource;
}

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
void Z3DEllipsoidRenderer::renderUsingOpengl()
{
  if (m_centers.empty())
    return;
  appendDefaultColors();

  GLUquadricObj* quadric = gluNewQuadric();
  for (size_t i=0; i<m_centers.size(); i+=4) {
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glm::mat4 m(m_axis1[i] * sizeScale() * 2.f,
                m_axis2[i] * sizeScale() * 2.f,
                m_axis3[i] * sizeScale() * 2.f,
                coordTransform() * m_centers[i]);
    glMultMatrixf(&m[0][0]);
    // overwrite material property setted by z3drendererbase
    if (m_useDynamicMaterial.get() && !m_specularAndShininess.empty()) {
      glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, m_specularAndShininess[i].w);
      glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, glm::value_ptr(glm::vec4(m_specularAndShininess[i].xyz(), 1.f)));
    }
    glColor4fv(glm::value_ptr(glm::vec4(m_ellipsoidColors[i].rgb(), m_ellipsoidColors[i].a * opacity())));
    gluSphere(quadric, .5, m_sphereSlicesStacks.get(), m_sphereSlicesStacks.get());
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
  }
  gluDeleteQuadric(quadric);
}

void Z3DEllipsoidRenderer::renderPickingUsingOpengl()
{
  if (m_centers.empty())
    return;
  if (m_ellipsoidPickingColors.empty() || m_centers.size() != m_ellipsoidPickingColors.size())
    return;
  GLUquadricObj* quadric = gluNewQuadric();
  for (size_t i=0; i<m_centers.size(); i+=4) {
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glm::mat4 m(m_axis1[i] * sizeScale() * 2.f,
                m_axis2[i] * sizeScale() * 2.f,
                m_axis3[i] * sizeScale() * 2.f,
                coordTransform() * m_centers[i]);
    glMultMatrixf(&m[0][0]);
    glColor4fv(glm::value_ptr(m_ellipsoidPickingColors[i]));
    gluSphere(quadric, 1., 12, 12/*m_sphereSlicesStacks.get(), m_sphereSlicesStacks.get()*/);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
  }
  gluDeleteQuadric(quadric);
}
#endif

void Z3DEllipsoidRenderer::render(Z3DEye eye)
{
  if (m_centers.empty()) {
    return;
  }
  appendDefaultColors();

  m_ellipsoidShaderGrp.bind();
  Z3DShaderProgram& shader = m_ellipsoidShaderGrp.get();
  m_rendererBase.setGlobalShaderParameters(shader, eye);
  setShaderParameters(shader);

  if (m_hardwareSupportVAO) {
    if (m_dataChanged) {
      m_VAO.bind();
      // set vertex data
      auto attr_T = shader.TAttributeLocation();
      GLint attr_a_specular_shininess = -1;
      if (m_useDynamicMaterial.get() && !m_specularAndShininess.empty()) {
        attr_a_specular_shininess = shader.specularShininessAttributeLocation();
      }
      auto attr_color = shader.colorAttributeLocation();
      auto attr_flags = shader.flagsAttributeLocation();

      glEnableVertexAttribArray(attr_T);
      m_VBOs.bind(GL_ARRAY_BUFFER, 0);
      glBufferData(GL_ARRAY_BUFFER, m_axis1.size() * 4 * sizeof(GLfloat), m_axis1.data(), GL_STATIC_DRAW);
      glVertexAttribPointer(attr_T, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_T + 1);
      m_VBOs.bind(GL_ARRAY_BUFFER, 1);
      glBufferData(GL_ARRAY_BUFFER, m_axis2.size() * 4 * sizeof(GLfloat), m_axis2.data(), GL_STATIC_DRAW);
      glVertexAttribPointer(attr_T + 1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_T + 2);
      m_VBOs.bind(GL_ARRAY_BUFFER, 2);
      glBufferData(GL_ARRAY_BUFFER, m_axis3.size() * 4 * sizeof(GLfloat), m_axis3.data(), GL_STATIC_DRAW);
      glVertexAttribPointer(attr_T + 2, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_T + 3);
      m_VBOs.bind(GL_ARRAY_BUFFER, 3);
      glBufferData(GL_ARRAY_BUFFER, m_centers.size() * 4 * sizeof(GLfloat), m_centers.data(), GL_STATIC_DRAW);
      glVertexAttribPointer(attr_T + 3, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      if (m_useDynamicMaterial.get() && !m_specularAndShininess.empty()) {
        glEnableVertexAttribArray(attr_a_specular_shininess);
        m_VBOs.bind(GL_ARRAY_BUFFER, 6);
        glBufferData(GL_ARRAY_BUFFER, m_specularAndShininess.size() * 4 * sizeof(GLfloat),
                     m_specularAndShininess.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(attr_a_specular_shininess, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
      }

      glEnableVertexAttribArray(attr_color);
      m_VBOs.bind(GL_ARRAY_BUFFER, 7);
      glBufferData(GL_ARRAY_BUFFER, m_ellipsoidColors.size() * 4 * sizeof(GLfloat), m_ellipsoidColors.data(),
                   GL_STATIC_DRAW);
      glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_flags);
      m_VBOs.bind(GL_ARRAY_BUFFER, 4);
      glBufferData(GL_ARRAY_BUFFER, m_allFlags.size() * sizeof(GLfloat), m_allFlags.data(), GL_STATIC_DRAW);
      glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

      m_VBOs.bind(GL_ELEMENT_ARRAY_BUFFER, 5);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indexs.size() * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      m_VAO.release();

      m_dataChanged = false;
    }

    m_VAO.bind();
    glDrawElements(GL_TRIANGLES, m_indexs.size(), GL_UNSIGNED_INT, nullptr);
    m_VAO.release();

  } else {
    // set vertex data
    auto attr_T = shader.TAttributeLocation();
    GLint attr_a_specular_shininess = -1;
    if (m_useDynamicMaterial.get() && !m_specularAndShininess.empty()) {
      attr_a_specular_shininess = shader.specularShininessAttributeLocation();
    }
    auto attr_color = shader.colorAttributeLocation();
    auto attr_flags = shader.flagsAttributeLocation();

    glEnableVertexAttribArray(attr_T);
    m_VBOs.bind(GL_ARRAY_BUFFER, 0);
    if (m_dataChanged) {
      glBufferData(GL_ARRAY_BUFFER, m_axis1.size() * 4 * sizeof(GLfloat), m_axis1.data(), GL_STATIC_DRAW);
    }
    glVertexAttribPointer(attr_T, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_T + 1);
    m_VBOs.bind(GL_ARRAY_BUFFER, 1);
    if (m_dataChanged) {
      glBufferData(GL_ARRAY_BUFFER, m_axis2.size() * 4 * sizeof(GLfloat), m_axis2.data(), GL_STATIC_DRAW);
    }
    glVertexAttribPointer(attr_T + 1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_T + 2);
    m_VBOs.bind(GL_ARRAY_BUFFER, 2);
    if (m_dataChanged) {
      glBufferData(GL_ARRAY_BUFFER, m_axis3.size() * 4 * sizeof(GLfloat), m_axis3.data(), GL_STATIC_DRAW);
    }
    glVertexAttribPointer(attr_T + 2, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_T + 3);
    m_VBOs.bind(GL_ARRAY_BUFFER, 3);
    if (m_dataChanged) {
      glBufferData(GL_ARRAY_BUFFER, m_centers.size() * 4 * sizeof(GLfloat), m_centers.data(), GL_STATIC_DRAW);
    }
    glVertexAttribPointer(attr_T + 3, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    if (m_useDynamicMaterial.get() && !m_specularAndShininess.empty()) {
      glEnableVertexAttribArray(attr_a_specular_shininess);
      m_VBOs.bind(GL_ARRAY_BUFFER, 6);
      if (m_dataChanged) {
        glBufferData(GL_ARRAY_BUFFER, m_specularAndShininess.size() * 4 * sizeof(GLfloat),
                     m_specularAndShininess.data(), GL_STATIC_DRAW);
      }
      glVertexAttribPointer(attr_a_specular_shininess, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    glEnableVertexAttribArray(attr_color);
    m_VBOs.bind(GL_ARRAY_BUFFER, 7);
    if (m_dataChanged) {
      glBufferData(GL_ARRAY_BUFFER, m_ellipsoidColors.size() * 4 * sizeof(GLfloat), m_ellipsoidColors.data(),
                   GL_STATIC_DRAW);
    }
    glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_flags);
    m_VBOs.bind(GL_ARRAY_BUFFER, 4);
    if (m_dataChanged) {
      glBufferData(GL_ARRAY_BUFFER, m_allFlags.size() * sizeof(GLfloat), m_allFlags.data(), GL_STATIC_DRAW);
    }
    glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

    m_VBOs.bind(GL_ELEMENT_ARRAY_BUFFER, 5);
    if (m_dataChanged) {
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indexs.size() * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);
    }

    glDrawElements(GL_TRIANGLES, m_indexs.size(), GL_UNSIGNED_INT, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(attr_T);
    glDisableVertexAttribArray(attr_T + 1);
    glDisableVertexAttribArray(attr_T + 2);
    glDisableVertexAttribArray(attr_T + 3);
    if (m_useDynamicMaterial.get() && !m_specularAndShininess.empty()) {
      glDisableVertexAttribArray(attr_a_specular_shininess);
    }
    glDisableVertexAttribArray(attr_color);
    glDisableVertexAttribArray(attr_flags);

    m_dataChanged = false;
  }

  m_ellipsoidShaderGrp.release();
}

void Z3DEllipsoidRenderer::renderPicking(Z3DEye eye)
{
  if (m_centers.empty()) {
    return;
  }

  if (m_ellipsoidPickingColors.empty() || m_centers.size() != m_ellipsoidPickingColors.size()) {
    return;
  }

  m_ellipsoidShaderGrp.bind();
  Z3DShaderProgram& shader = m_ellipsoidShaderGrp.get();
  m_rendererBase.setGlobalShaderParameters(shader, eye);
  setPickingShaderParameters(shader);

  if (m_hardwareSupportVAO) {
    if (m_pickingDataChanged) {
      m_pickingVAO.bind();
      // set vertex data
      auto attr_T = shader.TAttributeLocation();
      auto attr_color = shader.colorAttributeLocation();
      auto attr_flags = shader.flagsAttributeLocation();

      glEnableVertexAttribArray(attr_T);
      if (m_dataChanged) {
        m_pickingVBOs.bind(GL_ARRAY_BUFFER, 0);
        glBufferData(GL_ARRAY_BUFFER, m_axis1.size() * 4 * sizeof(GLfloat), m_axis1.data(), GL_STATIC_DRAW);
      } else {
        m_VBOs.bind(GL_ARRAY_BUFFER, 0);
      }
      glVertexAttribPointer(attr_T, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_T + 1);
      if (m_dataChanged) {
        m_pickingVBOs.bind(GL_ARRAY_BUFFER, 1);
        glBufferData(GL_ARRAY_BUFFER, m_axis2.size() * 4 * sizeof(GLfloat), m_axis2.data(), GL_STATIC_DRAW);
      } else {
        m_VBOs.bind(GL_ARRAY_BUFFER, 1);
      }
      glVertexAttribPointer(attr_T + 1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_T + 2);
      if (m_dataChanged) {
        m_pickingVBOs.bind(GL_ARRAY_BUFFER, 2);
        glBufferData(GL_ARRAY_BUFFER, m_axis3.size() * 4 * sizeof(GLfloat), m_axis3.data(), GL_STATIC_DRAW);
      } else {
        m_VBOs.bind(GL_ARRAY_BUFFER, 2);
      }
      glVertexAttribPointer(attr_T + 2, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_T + 3);
      if (m_dataChanged) {
        m_pickingVBOs.bind(GL_ARRAY_BUFFER, 3);
        glBufferData(GL_ARRAY_BUFFER, m_centers.size() * 4 * sizeof(GLfloat), m_centers.data(), GL_STATIC_DRAW);
      } else {
        m_VBOs.bind(GL_ARRAY_BUFFER, 3);
      }
      glVertexAttribPointer(attr_T + 3, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_color);
      m_pickingVBOs.bind(GL_ARRAY_BUFFER, 6);
      glBufferData(GL_ARRAY_BUFFER, m_ellipsoidPickingColors.size() * 4 * sizeof(GLfloat),
                   m_ellipsoidPickingColors.data(), GL_STATIC_DRAW);
      glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_flags);
      if (m_dataChanged) {
        m_pickingVBOs.bind(GL_ARRAY_BUFFER, 4);
        glBufferData(GL_ARRAY_BUFFER, m_allFlags.size() * sizeof(GLfloat), m_allFlags.data(), GL_STATIC_DRAW);
      } else {
        m_VBOs.bind(GL_ARRAY_BUFFER, 4);
      }
      glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

      if (m_dataChanged) {
        m_pickingVBOs.bind(GL_ELEMENT_ARRAY_BUFFER, 5);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indexs.size() * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);
      } else {
        m_VBOs.bind(GL_ELEMENT_ARRAY_BUFFER, 5);
      }

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      m_pickingVAO.release();

      m_pickingDataChanged = false;
    }

    m_pickingVAO.bind();
    glDrawElements(GL_TRIANGLES, m_indexs.size(), GL_UNSIGNED_INT, nullptr);
    m_pickingVAO.release();

  } else {
    // set vertex data
    auto attr_T = shader.TAttributeLocation();
    auto attr_color = shader.colorAttributeLocation();
    auto attr_flags = shader.flagsAttributeLocation();

    glEnableVertexAttribArray(attr_T);
    if (m_dataChanged) {
      m_pickingVBOs.bind(GL_ARRAY_BUFFER, 0);
      if (m_pickingDataChanged) {
        glBufferData(GL_ARRAY_BUFFER, m_axis1.size() * 4 * sizeof(GLfloat), m_axis1.data(), GL_STATIC_DRAW);
      }
    } else {
      m_VBOs.bind(GL_ARRAY_BUFFER, 0);
    }
    glVertexAttribPointer(attr_T, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_T + 1);
    if (m_dataChanged) {
      m_pickingVBOs.bind(GL_ARRAY_BUFFER, 1);
      if (m_pickingDataChanged) {
        glBufferData(GL_ARRAY_BUFFER, m_axis2.size() * 4 * sizeof(GLfloat), m_axis2.data(), GL_STATIC_DRAW);
      }
    } else {
      m_VBOs.bind(GL_ARRAY_BUFFER, 1);
    }
    glVertexAttribPointer(attr_T + 1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_T + 2);
    if (m_dataChanged) {
      m_pickingVBOs.bind(GL_ARRAY_BUFFER, 2);
      if (m_pickingDataChanged) {
        glBufferData(GL_ARRAY_BUFFER, m_axis3.size() * 4 * sizeof(GLfloat), m_axis3.data(), GL_STATIC_DRAW);
      }
    } else {
      m_VBOs.bind(GL_ARRAY_BUFFER, 2);
    }
    glVertexAttribPointer(attr_T + 2, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_T + 3);
    if (m_dataChanged) {
      m_pickingVBOs.bind(GL_ARRAY_BUFFER, 3);
      if (m_pickingDataChanged) {
        glBufferData(GL_ARRAY_BUFFER, m_centers.size() * 4 * sizeof(GLfloat), m_centers.data(), GL_STATIC_DRAW);
      }
    } else {
      m_VBOs.bind(GL_ARRAY_BUFFER, 3);
    }
    glVertexAttribPointer(attr_T + 3, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_color);
    m_pickingVBOs.bind(GL_ARRAY_BUFFER, 6);
    if (m_pickingDataChanged) {
      glBufferData(GL_ARRAY_BUFFER, m_ellipsoidPickingColors.size() * 4 * sizeof(GLfloat),
                   m_ellipsoidPickingColors.data(), GL_STATIC_DRAW);
    }
    glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_flags);
    if (m_dataChanged) {
      m_pickingVBOs.bind(GL_ARRAY_BUFFER, 4);
      if (m_pickingDataChanged) {
        glBufferData(GL_ARRAY_BUFFER, m_allFlags.size() * sizeof(GLfloat), m_allFlags.data(), GL_STATIC_DRAW);
      }
    } else {
      m_VBOs.bind(GL_ARRAY_BUFFER, 4);
    }
    glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

    if (m_dataChanged) {
      m_pickingVBOs.bind(GL_ELEMENT_ARRAY_BUFFER, 5);
      if (m_pickingDataChanged) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indexs.size() * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);
      }
    } else {
      m_VBOs.bind(GL_ELEMENT_ARRAY_BUFFER, 5);
    }

    glDrawElements(GL_TRIANGLES, m_indexs.size(), GL_UNSIGNED_INT, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(attr_T + 0);
    glDisableVertexAttribArray(attr_T + 1);
    glDisableVertexAttribArray(attr_T + 2);
    glDisableVertexAttribArray(attr_T + 3);
    glDisableVertexAttribArray(attr_color);
    glDisableVertexAttribArray(attr_flags);

    m_pickingDataChanged = false;
  }

  m_ellipsoidShaderGrp.release();
}

void Z3DEllipsoidRenderer::appendDefaultColors()
{
  if (m_ellipsoidColors.size() < m_centers.size()) {
    for (size_t i = m_ellipsoidColors.size(); i < m_centers.size(); ++i) {
      m_ellipsoidColors.emplace_back(0.f, 0.f, 0.f, 1.f);
    }
  }
}

} // namespace nim
