#include "z3dviewsettingparamops.h"

#include "z3drenderingengine.h"
#include "z3dtransformparameter.h"
#include "zlog.h"
#include "znumericparameter.h"
#include "zoptionparameter.h"
#include "zparameter.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace nim {

ZParameter* Z3DViewSettingParamOps::findTargetParam(const std::vector<ZParameter*>& params,
                                                    const QString& requestedJsonKey)
{
  const QString jsonKey = requestedJsonKey.trimmed();
  if (jsonKey.isEmpty()) {
    return nullptr;
  }
  for (auto* p : params) {
    // RPC callers must use the canonical current jsonKey(). Legacy names are
    // supported only for file deserialization (scene/animation load).
    if (p && p->jsonKey() == jsonKey) {
      return p;
    }
  }
  return nullptr;
}

json::object Z3DViewSettingParamOps::getParamValues(Z3DRenderingEngine& engine, size_t boundId)
{
  const auto params = engine.parametersOfViewSetting(boundId);
  json::object out;
  for (auto* p : params) {
    if (p) {
      p->write(out);
    }
  }
  return out;
}

std::vector<Z3DViewSettingParamOps::ParameterMeta> Z3DViewSettingParamOps::listParams(Z3DRenderingEngine& engine,
                                                                                      size_t boundId)
{
  std::vector<ParameterMeta> out;
  const auto params = engine.parametersOfViewSetting(boundId);
  out.reserve(params.size());
  for (auto* p : params) {
    if (!p) {
      continue;
    }
    ParameterMeta meta;
    meta.jsonKey = p->jsonKey();
    meta.name = p->name();
    meta.type = p->type();
    meta.description = p->description();
    meta.supportsInterpolation = p->supportInterpolation();
    meta.valueSchema = p->valueSchema();
    out.push_back(std::move(meta));
  }
  return out;
}

std::vector<Z3DViewSettingParamOps::ValidateResult> Z3DViewSettingParamOps::validate(
  Z3DRenderingEngine& engine,
  const std::vector<SetParamData>& setParams)
{
  std::vector<ValidateResult> out;
  out.reserve(setParams.size());

  auto validateOne = [&](const SetParamData& sp) -> ValidateResult {
    ValidateResult r;
    r.jsonKey = sp.jsonKey.trimmed();

    if (r.jsonKey.isEmpty()) {
      r.ok = false;
      r.reason = "json_key_required";
      return r;
    }

    const size_t boundId = sp.id;
    const auto params = engine.parametersOfViewSetting(boundId);
    if (params.empty()) {
      r.ok = false;
      r.reason = QString("target_not_ready: id=%1").arg(boundId);
      return r;
    }

    ZParameter* target = findTargetParam(params, r.jsonKey);
    if (!target) {
      r.ok = false;
      r.reason = QString("json_key_not_found: id=%1 json_key=%2").arg(boundId).arg(r.jsonKey);
      return r;
    }

    json::value v = sp.value;
    const QString tstr = target->type();

    // Strict validation for option parameters (system boundary: reject early with a soft error)
    if (auto optSI = dynamic_cast<const ZStringIntOptionParameter*>(target)) {
      if (!v.is_string()) {
        r.ok = false;
        r.reason = QString("type_mismatch: expected string got %1").arg(QString::fromStdString(jsonTypeName(v)));
        return r;
      }
      const auto& bs = v.as_string();
      const QString label = QString::fromUtf8(bs.data(), static_cast<int>(bs.size()));
      if (!optSI->hasOption(label)) {
        r.ok = false;
        r.reason = QString("option_invalid: %1").arg(label);
        return r;
      }
      r.ok = true;
      r.hasNormalizedValue = true;
      r.normalizedValue = json::value_from(label);
      return r;
    }
    if (auto optSS = dynamic_cast<const ZStringStringOptionParameter*>(target)) {
      if (!v.is_string()) {
        r.ok = false;
        r.reason = QString("type_mismatch: expected string got %1").arg(QString::fromStdString(jsonTypeName(v)));
        return r;
      }
      const auto& bs = v.as_string();
      const QString label = QString::fromUtf8(bs.data(), static_cast<int>(bs.size()));
      if (!optSS->hasOption(label)) {
        r.ok = false;
        r.reason = QString("option_invalid: %1").arg(label);
        return r;
      }
      r.ok = true;
      r.hasNormalizedValue = true;
      r.normalizedValue = json::value_from(label);
      return r;
    }
    if (auto optII = dynamic_cast<const ZIntIntOptionParameter*>(target)) {
      if (!v.is_number()) {
        r.ok = false;
        r.reason = QString("type_mismatch: expected number got %1").arg(QString::fromStdString(jsonTypeName(v)));
        return r;
      }
      const int ival = static_cast<int>(std::floor(v.to_number<double>() + 0.5));
      if (!optII->hasOption(ival)) {
        r.ok = false;
        r.reason = QString("option_invalid: %1").arg(ival);
        return r;
      }
      r.ok = true;
      r.hasNormalizedValue = true;
      r.normalizedValue = json::value_from(ival);
      return r;
    }

    // Non-option types (light type checks; full schema enforcement happens in target->read during apply)
    const bool okType = [&]() -> bool {
      if (tstr == "Bool") {
        return v.is_bool();
      }
      if (tstr == "Int") {
        return v.is_number();
      }
      if (tstr == "Float" || tstr == "Double") {
        return v.is_number();
      }
      if (tstr == "Vec2" || tstr == "DVec2") {
        return v.is_array() && v.as_array().size() == 2;
      }
      if (tstr == "Vec3" || tstr == "DVec3") {
        return v.is_array() && v.as_array().size() == 3;
      }
      if (tstr == "Vec4" || tstr == "DVec4") {
        return v.is_array() && v.as_array().size() == 4;
      }
      if (tstr == "3DTransform" || tstr == "3DCamera") {
        return v.is_object();
      }
      // Other custom types: accept and defer to target->read during apply
      return true;
    }();

    if (!okType) {
      r.ok = false;
      r.reason = QString("type_mismatch: expected %1 got %2").arg(tstr, QString::fromStdString(jsonTypeName(v)));
      return r;
    }

    // Range clamp for numeric/vector when metadata is available.
    auto setNormalized = [&](json::value nv) {
      r.hasNormalizedValue = true;
      r.normalizedValue = std::move(nv);
    };
    bool normalized = false;

    if (auto dp = dynamic_cast<ZDoubleParameter*>(target)) {
      if (v.is_number()) {
        double x = v.to_number<double>();
        x = std::clamp(x, dp->rangeMin(), dp->rangeMax());
        setNormalized(x);
        normalized = true;
      }
    } else if (auto fp = dynamic_cast<ZFloatParameter*>(target)) {
      if (v.is_number()) {
        double x = v.to_number<double>();
        x = std::clamp(x, static_cast<double>(fp->rangeMin()), static_cast<double>(fp->rangeMax()));
        setNormalized(x);
        normalized = true;
      }
    } else if (auto ip = dynamic_cast<ZIntParameter*>(target)) {
      if (v.is_number()) {
        double x = v.to_number<double>();
        x = std::clamp(x, static_cast<double>(ip->rangeMin()), static_cast<double>(ip->rangeMax()));
        setNormalized(std::floor(x + 0.5));
        normalized = true;
      }
    } else if (auto v2 = dynamic_cast<ZVec2Parameter*>(target)) {
      if (v.is_array() && v.as_array().size() == 2) {
        auto mn = v2->rangeMin();
        auto mx = v2->rangeMax();
        json::array arr;
        for (size_t i = 0; i < 2; ++i) {
          double x = v.as_array()[i].is_number() ? v.as_array()[i].to_number<double>() : 0.0;
          x = std::clamp(x, static_cast<double>(mn[i]), static_cast<double>(mx[i]));
          arr.emplace_back(x);
        }
        setNormalized(arr);
        normalized = true;
      }
    } else if (auto v3 = dynamic_cast<ZVec3Parameter*>(target)) {
      if (v.is_array() && v.as_array().size() == 3) {
        auto mn = v3->rangeMin();
        auto mx = v3->rangeMax();
        json::array arr;
        for (size_t i = 0; i < 3; ++i) {
          double x = v.as_array()[i].is_number() ? v.as_array()[i].to_number<double>() : 0.0;
          x = std::clamp(x, static_cast<double>(mn[i]), static_cast<double>(mx[i]));
          arr.emplace_back(x);
        }
        setNormalized(arr);
        normalized = true;
      }
    } else if (auto v4 = dynamic_cast<ZVec4Parameter*>(target)) {
      if (v.is_array() && v.as_array().size() == 4) {
        auto mn = v4->rangeMin();
        auto mx = v4->rangeMax();
        json::array arr;
        for (size_t i = 0; i < 4; ++i) {
          double x = v.as_array()[i].is_number() ? v.as_array()[i].to_number<double>() : 0.0;
          x = std::clamp(x, static_cast<double>(mn[i]), static_cast<double>(mx[i]));
          arr.emplace_back(x);
        }
        setNormalized(arr);
        normalized = true;
      }
    }

    // Special handling for 3DTransform: accept plain field names and normalize to child jsonKeys.
    if (!normalized) {
      if (dynamic_cast<const Z3DTransformParameter*>(target)) {
        if (v.is_object()) {
          const auto& obj = v.as_object();
          json::object outObj;
          auto getVec = [&](const char* key1, const char* key2, size_t n) -> std::optional<json::array> {
            const json::value* pv = nullptr;
            if (auto it = obj.if_contains(key1)) {
              pv = &*it;
            } else if (auto it2 = obj.if_contains(key2)) {
              pv = &*it2;
            } else {
              return std::nullopt;
            }
            if (!pv->is_array() || pv->as_array().size() != n) {
              return std::nullopt;
            }
            json::array arr;
            for (size_t i = 0; i < n; ++i) {
              const auto& el = pv->as_array()[i];
              if (!el.is_number()) {
                return std::nullopt;
              }
              arr.emplace_back(el.to_number<double>());
            }
            return arr;
          };
          if (auto a = getVec("Scale", "Scale Vec3", 3)) {
            outObj["Scale Vec3"] = *a;
          }
          if (auto a = getVec("Translation", "Translation Vec3", 3)) {
            outObj["Translation Vec3"] = *a;
          }
          if (auto a = getVec("Rotation", "Rotation Vec4", 4)) {
            outObj["Rotation Vec4"] = *a;
          }
          // Prefer canonical "Rotation Center Vec3"; accept synonyms
          if (auto a = getVec("Rotation Center", "Rotation Center Vec3", 3)) {
            outObj["Rotation Center Vec3"] = *a;
          } else if (auto a2 = getVec("Center", "Center Vec3", 3)) {
            outObj["Rotation Center Vec3"] = *a2;
          }
          setNormalized(outObj);
        } else {
          setNormalized(v);
        }
      } else {
        setNormalized(v);
      }
    }

    r.ok = true;
    r.reason.clear();
    return r;
  };

  for (const auto& sp : setParams) {
    out.push_back(validateOne(sp));
  }
  return out;
}

std::pair<bool, std::string> Z3DViewSettingParamOps::apply(Z3DRenderingEngine& engine,
                                                           const std::vector<SetParamData>& setParams)
{
  for (const auto& sp : setParams) {
    const size_t boundId = sp.id;
    const auto params = engine.parametersOfViewSetting(boundId);
    const QString jsonKey = sp.jsonKey.trimmed();
    if (jsonKey.isEmpty()) {
      return {false,
              std::string("apply_scene_params: json_key_required: id=") + std::to_string(boundId)};
    }
    if (params.empty()) {
      return {false,
              std::string("apply_scene_params: target_not_ready: id=") + std::to_string(boundId)};
    }

    ZParameter* target = findTargetParam(params, jsonKey);
    if (!target) {
      return {
        false,
        std::string("apply_scene_params: json_key_not_found: id=") + std::to_string(boundId) + " json_key=" +
          jsonKey.toStdString()};
    }

    json::value v = sp.value;
    json::object j;
    if (dynamic_cast<const Z3DTransformParameter*>(target)) {
      if (v.is_object()) {
        const auto& obj = v.as_object();
        json::object outObj;
        auto mapField = [&](const char* key1, const char* key2) {
          if (auto it = obj.if_contains(key1)) {
            outObj[key2] = *it;
          } else if (auto it2 = obj.if_contains(key2)) {
            outObj[key2] = *it2;
          }
        };
        mapField("Scale", "Scale Vec3");
        mapField("Translation", "Translation Vec3");
        mapField("Rotation", "Rotation Vec4");
        // Canonical center key is "Rotation Center Vec3"; accept synonyms
        mapField("Rotation Center", "Rotation Center Vec3");
        mapField("Center", "Rotation Center Vec3");
        mapField("Center Vec3", "Rotation Center Vec3");
        j[jsonKey.toStdString()] = outObj;
      } else {
        j[jsonKey.toStdString()] = v;
      }
    } else {
      j[jsonKey.toStdString()] = v;
    }

    try {
      target->read(j);
    }
    catch (const std::exception& e) {
      std::string err =
        std::string("apply_scene_params: exception while reading json_key=") + jsonKey.toStdString() + " (" +
        target->type().toStdString() + "): " + e.what();
      LOG(WARNING) << err;
      return {false, err};
    }
    catch (...) {
      std::string err =
        std::string("apply_scene_params: unknown exception while reading json_key=") + jsonKey.toStdString() + " (" +
        target->type().toStdString() + ")";
      LOG(WARNING) << err;
      return {false, err};
    }
  }
  return {true, ""};
}

} // namespace nim
