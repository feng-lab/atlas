#pragma once

#include "zlog.h"

namespace nim::neutube {

using ScoreFunctionLegacyLike = double (*)(const double* var, const void* param);
using ValidatorFunctionLegacyLike = void (*)(double* var,
                                             const double* varMin,
                                             const double* varMax,
                                             const void* param);

// C++ port of tz_cont_fun.h::Variable_Set.
//
// Notes:
// - `var` points to the *full* variable array.
// - `varIndex` lists the indices of the active variables inside `var`.
// - `link` optionally encodes variable linking using the legacy base-100 scheme.
struct VariableSet
{
  double* var = nullptr;
  const int* varIndex = nullptr;
  const int* link = nullptr;
  int nvar = 0;
};

inline void variableSetAddLinkLegacyLike(int* link, int master, int slave)
{
  CHECK(link != nullptr);
  link[master] = link[master] * 100 + slave + 1;
}

// Port of tz_cont_fun.c::Variable_Set_Update_Link().
inline void variableSetUpdateLinkLegacyLike(VariableSet& vs)
{
  if (vs.link == nullptr) {
    return;
  }

  CHECK(vs.var != nullptr);
  CHECK(vs.varIndex != nullptr);

  for (int i = 0; i < vs.nvar; ++i) {
    int remain = vs.link[i];
    while (remain > 0) {
      const int slaveIndex = remain % 100 - 1;
      CHECK(slaveIndex >= 0);
      vs.var[slaveIndex] = vs.var[vs.varIndex[i]];
      remain /= 100;
    }
  }
}

// C++ port of tz_cont_fun.h::Continuous_Function.
struct ContinuousFunction
{
  ScoreFunctionLegacyLike f = nullptr;
  ValidatorFunctionLegacyLike v = nullptr;
  const double* varMin = nullptr;
  const double* varMax = nullptr;
};

} // namespace nim::neutube
