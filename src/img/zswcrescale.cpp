#include "zswcrescale.h"

#include "zcancellation.h"
#include "zexception.h"
#include "zswc.h"

#include <cmath>

namespace nim {

namespace {

void translateSwc(ZSwc& swc, double dx, double dy, double dz)
{
  if (dx == 0.0 && dy == 0.0 && dz == 0.0) {
    return;
  }

  for (auto it = swc.begin(); it != swc.end(); ++it) {
    it->x += dx;
    it->y += dy;
    it->z += dz;
  }
}

void scaleSwc(ZSwc& swc, double scaleX, double scaleY, double scaleZ, bool scaleRadius)
{
  if (scaleX == 1.0 && scaleY == 1.0 && scaleZ == 1.0) {
    return;
  }

  const double radiusScale = std::sqrt(scaleX * scaleY);

  for (auto it = swc.begin(); it != swc.end(); ++it) {
    it->x *= scaleX;
    it->y *= scaleY;
    it->z *= scaleZ;
    if (scaleRadius) {
      it->radius *= radiusScale;
    }
  }
}

} // namespace

void ZSwcRescale::doWork()
{
  if (m_outputSwcFilename.trimmed().isEmpty()) {
    throw ZException("Output SWC file can not be empty.");
  }
  if (m_inputSwcFilename.trimmed().isEmpty()) {
    throw ZException("Input SWC file can not be empty.");
  }

  maybeCancel(m_cancellationToken);

  ZSwc tree(m_inputSwcFilename);

  maybeCancel(m_cancellationToken);

  translateSwc(tree, m_settings.preTranslateX, m_settings.preTranslateY, m_settings.preTranslateZ);

  maybeCancel(m_cancellationToken);

  scaleSwc(tree, m_settings.scaleX, m_settings.scaleY, m_settings.scaleZ, m_settings.scaleRadius);

  maybeCancel(m_cancellationToken);

  translateSwc(tree, m_settings.postTranslateX, m_settings.postTranslateY, m_settings.postTranslateZ);
  maybeCancel(m_cancellationToken);

  tree.save(m_outputSwcFilename);
  reportProgress(1.0);
}

void ZSwcRescale::read(const json::object& jo)
{
  m_inputSwcFilename = json::value_to<QString>(jo.at("input_swc"));
  m_outputSwcFilename = json::value_to<QString>(jo.at("output_swc"));

  const auto& s = jo.at("settings").as_object();
  m_settings.preTranslateX = s.at("pre_translate_x").to_number<double>();
  m_settings.preTranslateY = s.at("pre_translate_y").to_number<double>();
  m_settings.preTranslateZ = s.at("pre_translate_z").to_number<double>();

  m_settings.scaleX = s.at("scale_x").to_number<double>();
  m_settings.scaleY = s.at("scale_y").to_number<double>();
  m_settings.scaleZ = s.at("scale_z").to_number<double>();

  m_settings.postTranslateX = s.at("post_translate_x").to_number<double>();
  m_settings.postTranslateY = s.at("post_translate_y").to_number<double>();
  m_settings.postTranslateZ = s.at("post_translate_z").to_number<double>();

  m_settings.scaleRadius = json::value_to<bool>(s.at("scale_radius"));
}

void ZSwcRescale::write(json::object& jo) const
{
  jo["input_swc"] = json::value_from(m_inputSwcFilename);
  jo["output_swc"] = json::value_from(m_outputSwcFilename);

  json::object s;
  s["pre_translate_x"] = json::value_from(m_settings.preTranslateX);
  s["pre_translate_y"] = json::value_from(m_settings.preTranslateY);
  s["pre_translate_z"] = json::value_from(m_settings.preTranslateZ);

  s["scale_x"] = json::value_from(m_settings.scaleX);
  s["scale_y"] = json::value_from(m_settings.scaleY);
  s["scale_z"] = json::value_from(m_settings.scaleZ);

  s["post_translate_x"] = json::value_from(m_settings.postTranslateX);
  s["post_translate_y"] = json::value_from(m_settings.postTranslateY);
  s["post_translate_z"] = json::value_from(m_settings.postTranslateZ);

  s["scale_radius"] = json::value_from(m_settings.scaleRadius);

  jo["settings"] = std::move(s);
}

} // namespace nim
