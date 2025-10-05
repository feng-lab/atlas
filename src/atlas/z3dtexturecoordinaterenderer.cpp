#include "z3dtexturecoordinaterenderer.h"

#include "z3dgl.h"
#include "zmesh.h"
#include "zlog.h"

namespace nim {

Z3DTextureCoordinateRenderer::Z3DTextureCoordinateRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_mesh(nullptr)
  , m_dataChanged(false)
{
  createResources(m_rendererBase.activeBackend());
}

void Z3DTextureCoordinateRenderer::compile()
{
  if (m_rendererBase.activeBackend() != RenderBackend::OpenGL) {
    return;
  }
  DCHECK(m_renderTextureCoordinateShader != nullptr);
  m_renderTextureCoordinateShader->setHeaderAndRebuild(m_rendererBase.generateHeader());
}

void Z3DTextureCoordinateRenderer::render(Z3DEye eye)
{
  if (!m_mesh || m_mesh->vertices().empty() || m_mesh->numVertices() != m_mesh->num3DTextureCoordinates()) {
    return;
  }

  const std::vector<glm::vec3>& vertices = m_mesh->vertices();
  const std::vector<glm::vec3>& texCoords = m_mesh->textureCoordinates3D();
  const std::vector<GLuint>& triangleIndexes = m_mesh->indices();

  m_renderTextureCoordinateShader->bind();
  m_rendererBase.setGlobalShaderParameters(*m_renderTextureCoordinateShader, eye);

  if (m_useVAO) {
    if (m_dataChanged) {
      m_VAO->bind();
      auto attr_vertex = m_renderTextureCoordinateShader->vertexAttributeLocation();
      auto attr_3dTexCoord0 = m_renderTextureCoordinateShader->tex3dCoord0AttributeLocation();

      size_t bufIdx = 0;
      glEnableVertexAttribArray(attr_vertex);
      m_VBOs->bind(GL_ARRAY_BUFFER, bufIdx++);
      glBufferData(GL_ARRAY_BUFFER, vertices.size() * 3 * sizeof(GLfloat), vertices.data(), GL_STATIC_DRAW);
      glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_3dTexCoord0);
      m_VBOs->bind(GL_ARRAY_BUFFER, bufIdx++);
      glBufferData(GL_ARRAY_BUFFER, texCoords.size() * 3 * sizeof(GLfloat), texCoords.data(), GL_STATIC_DRAW);
      glVertexAttribPointer(attr_3dTexCoord0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

      if (!triangleIndexes.empty()) {
        m_VBOs->bind(GL_ELEMENT_ARRAY_BUFFER, bufIdx++);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     triangleIndexes.size() * sizeof(GLuint),
                     triangleIndexes.data(),
                     GL_STATIC_DRAW);
      }

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      m_VAO->release();

      m_dataChanged = false;
    }

    m_VAO->bind();
    if (triangleIndexes.empty()) {
      glDrawArrays(toGLType(m_mesh->type()), 0, vertices.size());
    } else {
      glDrawElements(toGLType(m_mesh->type()), triangleIndexes.size(), GL_UNSIGNED_INT, nullptr);
    }
    m_VAO->release();

  } else {
    auto attr_vertex = m_renderTextureCoordinateShader->vertexAttributeLocation();
    auto attr_3dTexCoord0 = m_renderTextureCoordinateShader->tex3dCoord0AttributeLocation();

    size_t bufIdx = 0;
    glEnableVertexAttribArray(attr_vertex);
    m_VBOs->bind(GL_ARRAY_BUFFER, bufIdx++);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * 3 * sizeof(GLfloat), vertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_3dTexCoord0);
    m_VBOs->bind(GL_ARRAY_BUFFER, bufIdx++);
    glBufferData(GL_ARRAY_BUFFER, texCoords.size() * 3 * sizeof(GLfloat), texCoords.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(attr_3dTexCoord0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    if (triangleIndexes.empty()) {
      glDrawArrays(toGLType(m_mesh->type()), 0, vertices.size());
    } else {
      m_VBOs->bind(GL_ELEMENT_ARRAY_BUFFER, bufIdx++);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                   triangleIndexes.size() * sizeof(GLuint),
                   triangleIndexes.data(),
                   GL_STATIC_DRAW);
      glDrawElements(toGLType(m_mesh->type()), triangleIndexes.size(), GL_UNSIGNED_INT, nullptr);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glDisableVertexAttribArray(attr_vertex);
    glDisableVertexAttribArray(attr_3dTexCoord0);
  }

  m_renderTextureCoordinateShader->release();
}

void Z3DTextureCoordinateRenderer::createResources(RenderBackend backend)
{
  if (backend != RenderBackend::OpenGL) {
    return;
  }
  m_renderTextureCoordinateShader = std::make_unique<Z3DShaderProgram>();
  m_renderTextureCoordinateShader->loadFromSourceFile("transform_with_3dtexture.vert",
                                                     "render_3dtexture_coordinate.frag",
                                                     m_rendererBase.generateHeader());

  m_VBOs = std::make_unique<Z3DVertexBufferObject>(3);
  m_VAO = std::make_unique<Z3DVertexArrayObject>(1);
  m_dataChanged = true;
}

void Z3DTextureCoordinateRenderer::destroyResources()
{
  m_renderTextureCoordinateShader.reset();
  m_VBOs.reset();
  m_VAO.reset();
}

} // namespace nim
