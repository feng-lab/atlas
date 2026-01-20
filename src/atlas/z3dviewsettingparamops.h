#pragma once

#include "z3dtypes.h"
#include "zjson.h"

#include <QString>
#include <string>
#include <utility>
#include <vector>

namespace nim {

class Z3DRenderingEngine;
class ZParameter;

// Helpers for reading/validating/applying view-setting parameters (camera/object/global) on the rendering thread.
//
// This is used by RPC, but intentionally does not depend on protobuf types so it can be reused by other
// programmatic entry points while keeping zrpcservice itself dispatch-only.
class Z3DViewSettingParamOps
{
public:
  struct ParameterMeta
  {
    QString jsonKey;
    QString name;
    QString type;
    QString description;
    bool supportsInterpolation = false;
    json::object valueSchema;
  };

  struct SetParamData
  {
    size_t id = 0; // 0=camera; 1=background; 2=axis; 3=global/lighting; >=4 object id
    // Canonical ZParameter::jsonKey() for this scope. Required for all scopes (including camera).
    QString jsonKey;
    json::value value;
  };

  struct ValidateResult
  {
    QString jsonKey; // original key as provided by caller (may be empty for camera)
    bool ok = false;
    QString reason; // stable token: json_key_not_found, type_mismatch, option_invalid, ...
    bool hasNormalizedValue = false;
    json::value normalizedValue;
  };

  [[nodiscard]] static json::object getParamValues(Z3DRenderingEngine& engine, size_t boundId);

  [[nodiscard]] static std::vector<ParameterMeta> listParams(Z3DRenderingEngine& engine, size_t boundId);

  [[nodiscard]] static std::vector<ValidateResult> validate(Z3DRenderingEngine& engine,
                                                            const std::vector<SetParamData>& setParams);

  [[nodiscard]] static std::pair<bool, std::string> apply(Z3DRenderingEngine& engine,
                                                          const std::vector<SetParamData>& setParams);

private:
  [[nodiscard]] static ZParameter* findTargetParam(const std::vector<ZParameter*>& params,
                                                   const QString& requestedJsonKey);
};

} // namespace nim
