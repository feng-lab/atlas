#include "z3dshaderprogram.h"

#include "z3dgl.h"
#include "zlog.h"
#include "z3dshadermanager.h"
#include "zexception.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace nim {

namespace {

struct FixedAttributeBinding
{
  std::string_view name;
  GLuint location;
};

// OpenGL links each hook-specific shader program independently, while Atlas caches VAOs/VBOs
// across normal/transparent hook variants. Reserve stable semantic slots so a depth-only hook
// can skip using an attribute without making VAO setup call glEnableVertexAttribArray(-1).
//
// Locations are allowed to alias only when the names are not active together in current GL
// shaders. In particular, keep texture coordinates distinct from attr_color: almag.vert uses
// attr_color and attr_2dTexCoord0 in the same program.
constexpr std::array kFixedAttributeBindings{
  FixedAttributeBinding{"attr_vertex",             0u},
  FixedAttributeBinding{"attr_origin",             0u},
  FixedAttributeBinding{"attr_p0",                 0u},
  FixedAttributeBinding{"attr_T",                  0u}, // mat4 consumes locations 0..3
  FixedAttributeBinding{"attr_normal",             1u},
  FixedAttributeBinding{"attr_axis",               1u},
  FixedAttributeBinding{"attr_p1",                 1u},
  FixedAttributeBinding{"attr_color",              4u},
  FixedAttributeBinding{"attr_p0color",            4u},
  FixedAttributeBinding{"attr_flags",              5u},
  FixedAttributeBinding{"attr_specular_shininess", 6u},
  FixedAttributeBinding{"attr_color2",             6u},
  FixedAttributeBinding{"attr_p1color",            6u},
  FixedAttributeBinding{"attr_1dTexCoord0",        7u},
  FixedAttributeBinding{"attr_2dTexCoord0",        8u},
  FixedAttributeBinding{"attr_3dTexCoord0",        9u},
};

std::optional<GLuint> fixedAttributeLocation(std::string_view name)
{
  for (const auto& binding : kFixedAttributeBindings) {
    if (binding.name == name) {
      return binding.location;
    }
  }
  return std::nullopt;
}

void bindFixedAttributeLocations(GLuint programId)
{
  for (const auto& binding : kFixedAttributeBindings) {
    glBindAttribLocation(programId, binding.location, binding.name.data());
  }
}

} // namespace

Z3DShaderProgram::Z3DShaderProgram()
{
  m_id = glCreateProgram();
  if (!m_id) {
    throw ZException("Z3DShaderProgram: Could not create shader program");
  }
}

Z3DShaderProgram::~Z3DShaderProgram()
{
  glDeleteProgram(m_id);
}

void Z3DShaderProgram::addShader(Z3DShader& shader)
{
  if (contains(m_shaders, &shader)) {
    return;
  }
  if (m_context != shader.context()) {
    throw ZException("Z3DShaderProgram: Add shader failed as program and shader are not associated with same context");
  }
  glAttachShader(m_id, shader.shaderId());
  m_linked = false;
  m_shaders.push_back(&shader);
}

void Z3DShaderProgram::addShaderFromSourceCode(Z3DShader::Type type, const char* source)
{
  CHECK(source);
  m_anonShaders.emplace_back(std::make_unique<Z3DShader>(type));
  m_anonShaders.back()->compileSourceCode(source);
  addShader(*m_anonShaders.back().get());
}

void Z3DShaderProgram::removeAllShaders()
{
  for (const auto& shader : m_shaders) {
    glDetachShader(m_id, shader->shaderId());
  }
  for (const auto& shader : m_anonShaders) {
    glDetachShader(m_id, shader->shaderId());
  }
  m_shaders.clear();
  m_anonShaders.clear();
  m_linked = false;
}

void Z3DShaderProgram::link()
{
  GLint value;
  if (m_shaders.empty()) {
    // If there are no explicit shaders, then it is possible that the
    // application added a program binary with glProgramBinaryOES(),
    // or otherwise populated the shaders itself. Check to see if the
    // program is already linked and bail out if so.
    value = 0;
    glGetProgramiv(m_id, GL_LINK_STATUS, &value);
    m_linked = (value != 0);
    if (m_linked) {
      storeUniformLocations();
      storeAttributeLocations();
    }
  }
  bindFixedAttributeLocations(m_id);
  glLinkProgram(m_id);
  value = 0;
  glGetProgramiv(m_id, GL_LINK_STATUS, &value);
  m_linked = (value != 0);
  if (m_linked) {
    storeUniformLocations();
    storeAttributeLocations();
  } else {
    value = 0;
    glGetProgramiv(m_id, GL_INFO_LOG_LENGTH, &value);
    if (value > 1) {
      std::vector<char> logbuf(value);
      GLint len;
      glGetProgramInfoLog(m_id, value, &len, logbuf.data());
      throw ZException(fmt::format("Z3DShaderProgram::Link: {}", logbuf.data()));
    } else {
      throw ZException("Z3DShaderProgram::Link: failed");
    }
  }
}

void Z3DShaderProgram::bind()
{
  if (!m_linked) {
    link();
  }
  m_textureUnitManager.reset();
  m_locToTextureUnit.clear();
  glUseProgram(m_id);
}

void Z3DShaderProgram::release() const
{
  glUseProgram(0);
}

void Z3DShaderProgram::bindTexture(const std::string& name, const Z3DTexture* texture)
{
  if (!texture) {
    return;
  }

  auto loc = uniformLocation(name);
  if (loc != -1) {
    GLenum textureEnum;
    GLint textureNumber;
    auto it = m_locToTextureUnit.find(loc);
    if (it == m_locToTextureUnit.end()) {
      m_textureUnitManager.nextAvailableUnit();
      textureEnum = m_textureUnitManager.currentUnitEnum();
      textureNumber = m_textureUnitManager.currentUnitNumber();
      m_locToTextureUnit[loc] = std::make_pair(textureEnum, textureNumber);
    } else {
      textureEnum = it->second.first;
      textureNumber = it->second.second;
    }
    glActiveTexture(textureEnum);
    texture->bind();
    setUniform(name, textureNumber);
    glActiveTexture(GL_TEXTURE0);
  }
}

void Z3DShaderProgram::bindTexture(const std::string& name, const Z3DTexture* texture, GLint minFilter, GLint magFilter)
{
  if (!texture) {
    return;
  }

  auto loc = uniformLocation(name);
  if (loc != -1) {
    GLenum textureEnum;
    GLint textureNumber;
    auto it = m_locToTextureUnit.find(loc);
    if (it == m_locToTextureUnit.end()) {
      m_textureUnitManager.nextAvailableUnit();
      textureEnum = m_textureUnitManager.currentUnitEnum();
      textureNumber = m_textureUnitManager.currentUnitNumber();
      m_locToTextureUnit[loc] = std::make_pair(textureEnum, textureNumber);
    } else {
      textureEnum = it->second.first;
      textureNumber = it->second.second;
    }
    glActiveTexture(textureEnum);
    texture->bind();
    texture->setFilter(minFilter, magFilter);
    setUniform(name, textureNumber);
    glActiveTexture(GL_TEXTURE0);
  }
}

void Z3DShaderProgram::bindTexture(const std::string& name, GLenum target, GLuint textureId)
{
  auto loc = uniformLocation(name);
  if (loc != -1) {
    GLenum textureEnum;
    GLint textureNumber;
    auto it = m_locToTextureUnit.find(loc);
    if (it == m_locToTextureUnit.end()) {
      m_textureUnitManager.nextAvailableUnit();
      textureEnum = m_textureUnitManager.currentUnitEnum();
      textureNumber = m_textureUnitManager.currentUnitNumber();
      m_locToTextureUnit[loc] = std::make_pair(textureEnum, textureNumber);
    } else {
      textureEnum = it->second.first;
      textureNumber = it->second.second;
    }
    glActiveTexture(textureEnum);
    glBindTexture(target, textureId);
    setUniform(name, textureNumber);
    glActiveTexture(GL_TEXTURE0);
  }
}

void Z3DShaderProgram::loadFromSourceFile(const QString& vertFilename,
                                          const QString& geomFilename,
                                          const QString& fragFilename,
                                          const std::string& header,
                                          const std::string& geomHeader)
{
  removeAllShaders();
  addShader(Z3DShaderManager::instance().shader(vertFilename, header, m_context));
  addShader(Z3DShaderManager::instance().shader(fragFilename, header, m_context));
  if (!geomFilename.isEmpty()) {
    addShader(Z3DShaderManager::instance().shader(geomFilename, geomHeader, m_context));
  }
  link();
  m_shaderFiles.clear();
  m_shaderFiles << vertFilename << fragFilename << geomFilename;
}

void Z3DShaderProgram::loadFromSourceFile(const QString& vertFilename,
                                          const QString& fragFilename,
                                          const std::string& header,
                                          const std::string& geomHeader)
{
  loadFromSourceFile(vertFilename, "", fragFilename, header, geomHeader);
}

void Z3DShaderProgram::loadFromSourceFile(const QStringList& shaderFilenames,
                                          const std::string& header,
                                          const std::string& geomHeader)
{
  removeAllShaders();
  for (const auto& shaderFilename : shaderFilenames) {
    if (shaderFilename.isEmpty()) {
      continue;
    }
    if (shaderFilename.endsWith(".geom", Qt::CaseInsensitive)) {
      addShader(Z3DShaderManager::instance().shader(shaderFilename, geomHeader, m_context));
    } else {
      addShader(Z3DShaderManager::instance().shader(shaderFilename, header, m_context));
    }
  }
  link();
  m_shaderFiles = shaderFilenames;
}

void Z3DShaderProgram::loadFromSourceCode(const std::vector<std::string>& vertSrcs,
                                          const std::vector<std::string>& geomSrcs,
                                          const std::vector<std::string>& fragSrcs,
                                          const std::string& header,
                                          const std::string& geomHeader)
{
  removeAllShaders();
  for (const auto& i : vertSrcs) {
    addShaderFromSourceCode(Z3DShader::Type::Vertex, header + i);
  }

  for (const auto& i : geomSrcs) {
    addShaderFromSourceCode(Z3DShader::Type::Geometry, geomHeader + i);
  }

  for (const auto& i : fragSrcs) {
    addShaderFromSourceCode(Z3DShader::Type::Fragment, header + i);
  }

  link();
}

void Z3DShaderProgram::loadFromSourceCode(const std::vector<std::string>& vertSrcs,
                                          const std::vector<std::string>& fragSrcs,
                                          const std::string& header,
                                          const std::string& geomHeader)
{
  loadFromSourceCode(vertSrcs, {}, fragSrcs, header, geomHeader);
}

void Z3DShaderProgram::setHeaderAndRebuild(const std::string& header, const std::string& geomHeader)
{
  loadFromSourceFile(m_shaderFiles, header, geomHeader);
}

int Z3DShaderProgram::uniformLocation(const std::string& name) const
{
  auto it = m_uniforms.find(name);
  if (it != m_uniforms.end()) {
    return it->second.location;
  }
  if (logUniformLocationError()) {
    LOG(WARNING) << "Failed to locate uniform: " << name;
  }
  return -1;
}

int Z3DShaderProgram::attributeLocation(const std::string& name) const
{
  auto it = m_attributes.find(name);
  if (it != m_attributes.end()) {
    return it->second.location;
  }
  if (const auto fixedLocation = fixedAttributeLocation(name)) {
    return static_cast<int>(*fixedLocation);
  }
  if (logUniformLocationError()) {
    LOG(WARNING) << "Failed to locate attribute: " << name;
  }
  return -1;
}

// void Z3DShaderProgram::setUniformValue(GLint loc, bool value)
//{
//   setUniformValue(loc, static_cast<GLint>(value));
// }

// void Z3DShaderProgram::setUniformValue(GLint loc, bool v1, bool v2)
//{
//   setUniformValue(loc, static_cast<GLint>(v1), static_cast<GLint>(v2));
// }

// void Z3DShaderProgram::setUniformValue(GLint loc, bool v1, bool v2, bool v3)
//{
//   setUniformValue(loc, static_cast<GLint>(v1), static_cast<GLint>(v2), static_cast<GLint>(v3));
// }

// void Z3DShaderProgram::setUniformValue(GLint loc, bool v1, bool v2, bool v3, bool v4)
//{
//   setUniformValue(loc, static_cast<GLint>(v1), static_cast<GLint>(v2), static_cast<GLint>(v3),
//   static_cast<GLint>(v4));
// }

// void Z3DShaderProgram::setUniformValue(const std::string& name, bool value)
//{
//   setUniformValue(name, static_cast<GLint>(value));
// }

// void Z3DShaderProgram::setUniformValue(const std::string& name, bool v1, bool v2)
//{
//   setUniformValue(name, static_cast<GLint>(v1), static_cast<GLint>(v2));
// }

// void Z3DShaderProgram::setUniformValue(const std::string& name, bool v1, bool v2, bool v3)
//{
//   setUniformValue(name, static_cast<GLint>(v1), static_cast<GLint>(v2), static_cast<GLint>(v3));
// }

// void Z3DShaderProgram::setUniformValue(const std::string& name, bool v1, bool v2, bool v3, bool v4)
//{
//   setUniformValue(name, static_cast<GLint>(v1), static_cast<GLint>(v2), static_cast<GLint>(v3),
//   static_cast<GLint>(v4));
// }

void Z3DShaderProgram::storeUniformLocations()
{
  m_uniforms.clear();
  GLint count;
  GLint maxLength;
  glGetProgramiv(programId(), GL_ACTIVE_UNIFORMS, &count);
  glGetProgramiv(programId(), GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxLength);
  std::vector<char> name(maxLength);
  for (auto i = 0; i < count; ++i) {
    Uniform u{};
    glGetActiveUniform(programId(), i, maxLength, nullptr, &u.size, &u.type, name.data());
    u.location = glGetUniformLocation(programId(), name.data());
    std::string nm(name.data());
    if (absl::EndsWith(nm, "[0]")) {
      nm.resize(nm.size() - 3); // chop last 3 characters
    }
    m_uniforms.insert_or_assign(nm, u);
  }

  std::map<std::string, Uniform>::const_iterator it;

  it = m_uniforms.find("screen_dim");
  m_screenDimUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("screen_dim_RCP");
  m_screenDimRCPUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("camera_position");
  m_cameraPositionUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("view_matrix");
  m_viewMatrixUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("view_matrix_inverse");
  m_viewMatrixInverseUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("projection_matrix");
  m_projectionMatrixUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("projection_matrix_inverse");
  m_projectionMatrixInverseUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("normal_matrix");
  m_normalMatrixUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("viewport_matrix");
  m_viewportMatrixUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("viewport_matrix_inverse");
  m_viewportMatrixInverseUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("projection_view_matrix");
  m_projectionViewMatrixUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("gamma");
  m_gammaUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("size_scale");
  m_sizeScaleUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("pos_transform");
  m_posTransformUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("pos_transform_normal_matrix");
  m_posTransformNormalMatrixUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("light_count");
  m_lightCountUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("lights_position");
  m_lightsPositionUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("lights_ambient");
  m_lightsAmbientUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("lights_diffuse");
  m_lightsDiffuseUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("lights_specular");
  m_lightsSpecularUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("lights_spotCutoff");
  m_lightsSpotCutoffUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("lights_attenuation");
  m_lightsAttenuationUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("lights_spotExponent");
  m_lightsSpotExponentUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("lights_spotDirection");
  m_lightsSpotDirectionUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("material_specular");
  m_materialSpecularUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("material_shininess");
  m_materialShininessUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("material_ambient");
  m_materialAmbientUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("ortho");
  m_orthoUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("scene_ambient");
  m_sceneAmbientUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("alpha");
  m_alphaUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("fog_color_top");
  m_fogColorTopUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("fog_color_bottom");
  m_fogColorBottomUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("fog_end");
  m_fogEndUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("fog_scale");
  m_fogScaleUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("fog_density_log2e");
  m_fogDensityLog2eUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("fog_density_density_log2e");
  m_fogDensityDensityLog2eUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("clip_planes");
  m_clipPlanesUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("lighting_enabled");
  m_lightingEnabledUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("color1");
  m_color1Uniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("color2");
  m_color2Uniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("line_width");
  m_lineWidthUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("box_correction");
  m_boxCorrectionUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("custom_color");
  m_customColorUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("use_custom_color");
  m_useCustomColorUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
  it = m_uniforms.find("region");
  m_regionUniform = (it == m_uniforms.end()) ? nullptr : &(it->second);
}

void Z3DShaderProgram::storeAttributeLocations()
{
  m_attributes.clear();
  GLint count;
  GLint maxLength;
  glGetProgramiv(programId(), GL_ACTIVE_ATTRIBUTES, &count);
  glGetProgramiv(programId(), GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, &maxLength);
  std::vector<char> name(maxLength);
  for (auto i = 0; i < count; ++i) {
    Attribute u{};
    glGetActiveAttrib(programId(), i, maxLength, nullptr, &u.size, &u.type, name.data());
    u.location = glGetAttribLocation(programId(), name.data());
    m_attributes.insert_or_assign(name.data(), u);
  }

  auto reserveFixedAttribute = [this](std::string_view name, GLuint location) {
    const std::string key(name);
    if (m_attributes.find(key) != m_attributes.end()) {
      return;
    }
    Attribute attr{};
    attr.location = static_cast<GLint>(location);
    m_attributes.emplace(key, attr);
  };

  for (const auto& binding : kFixedAttributeBindings) {
    reserveFixedAttribute(binding.name, binding.location);
  }

  std::map<std::string, Attribute>::const_iterator it;

  it = m_attributes.find("attr_vertex");
  m_vertexAttribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_normal");
  m_normalAttribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_origin");
  m_originAttribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_axis");
  m_axisAttribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_flags");
  m_flagsAttribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_color");
  m_colorAttribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_color2");
  m_color2Attribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_specular_shininess");
  m_specularShininessAttribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_1dTexCoord0");
  m_1dTexCoord0Attribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_2dTexCoord0");
  m_2dTexCoord0Attribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_3dTexCoord0");
  m_3dTexCoord0Attribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_p0");
  m_p0Attribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_p1");
  m_p1Attribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_p0color");
  m_p0ColorAttribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_p1color");
  m_p1ColorAttribute = (it == m_attributes.end()) ? nullptr : &(it->second);
  it = m_attributes.find("attr_T");
  m_TAttribute = (it == m_attributes.end()) ? nullptr : &(it->second);
}

} // namespace nim
