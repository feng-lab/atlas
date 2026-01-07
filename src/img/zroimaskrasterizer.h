#pragma once

#include "zglmutils.h"
#include "zimg.h"
#include <tuple>
#include <vector>

namespace nim {

enum class ZROIMaskShapeType
{
  Rect,
  Ellipse,
  Polygon,
  Spline,
  Line
};

struct ZROIMaskOperation2D
{
  bool isAdd = true;
  ZROIMaskShapeType type = ZROIMaskShapeType::Spline;
  std::vector<glm::dvec2> poly;
};

struct ZROIMaskRasterizerSettings
{
  // Supersampling factor. Higher values better approximate curve edges.
  // This is intentionally explicit/configurable to avoid hidden hard limits.
  int supersample = 5;

  // Applies to ZROIMaskShapeType::Line only (when the shape consists of a single Line op).
  double lineStrokeWidth = 2.0;

  // Optional scaling applied before rasterization (matches how ZROI scales painter paths).
  double scaleX = 1.0;
  double scaleY = 1.0;
};

class ZROIMaskRasterizer
{
public:
  // Rasterize a *single* shape described by ordered add/sub operations (mask-space boolean).
  // Returns a tight binary mask (values are 0/1) and the integer offset (x_start, y_start)
  // where the mask should be placed in the original coordinate system.
  [[nodiscard]] static std::tuple<ZImg, index_t, index_t>
  shapeToMask(const std::vector<ZROIMaskOperation2D>& shapeOps,
              const ZROIMaskRasterizerSettings& settings = ZROIMaskRasterizerSettings());
};

} // namespace nim

