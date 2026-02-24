#pragma once

#include "zneutubegeo3dcircle.h"
#include "zneutubeneuroseg.h"
#include "zneutubetracerecord.h"

#include <array>
#include <vector>

namespace nim {

class ZImg;

}

namespace nim::neutube {

struct Geo3dScalarField;

inline constexpr int LocalNeurosegNParamLegacyLike = 11;
inline constexpr int LocalNeurosegParamArraySizeLegacyLike = LocalNeurosegNParamLegacyLike + 1;
inline constexpr int LocsegFitWorkspaceMaxVarNumberLegacyLike = 20;

// Matches tz_local_neuroseg.h's compile-time constant:
//   const static int Neuropos_Reference = NEUROSEG_BOTTOM;
inline constexpr NeuroposReferenceLegacyLike NeuroposReference = NeuroposReferenceLegacyLike::Bottom;

// C++ port of tz_local_neuroseg.h::Local_Neuroseg (only fields used by tracing).
struct LocalNeuroseg
{
  Neuroseg seg{};
  std::array<double, 3> pos{};
};

// C++ port of tz_workspace.h::Locseg_Score_Workspace (alias of Receptor_Score_Workspace).
struct LocsegScoreWorkspace
{
  StackFitScore fs{};
  NeurosegFieldFunctionLegacyLike fieldFunc = nullptr;
  const ZImg* mask = nullptr;
};

// C++ port of tz_workspace.h::Locseg_Fit_Workspace (alias of Receptor_Fit_Workspace).
struct LocsegFitWorkspace
{
  std::array<int, LocsegFitWorkspaceMaxVarNumberLegacyLike> varIndex{};
  int nvar = 0;
  const int* varLink = nullptr;
  std::array<double, LocsegFitWorkspaceMaxVarNumberLegacyLike> varMin{};
  std::array<double, LocsegFitWorkspaceMaxVarNumberLegacyLike> varMax{};
  int posAdjust = 0;
  LocsegScoreWorkspace sws{};
};

// Port of tz_workspace.c::Default_Locseg_Fit_Workspace().
void defaultLocsegFitWorkspaceLegacyLike(LocsegFitWorkspace* ws);

// Port of tz_local_neuroseg.c::Set_Neuroseg_Position().
void setNeurosegPositionLegacyLike(LocalNeuroseg* locseg,
                                   const std::array<double, 3>& pos,
                                   NeuroposReferenceLegacyLike ref);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Bottom().
[[nodiscard]] std::array<double, 3> localNeurosegBottomLegacyLike(const LocalNeuroseg& locseg);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Center().
[[nodiscard]] std::array<double, 3> localNeurosegCenterLegacyLike(const LocalNeuroseg& locseg);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Top().
[[nodiscard]] std::array<double, 3> localNeurosegTopLegacyLike(const LocalNeuroseg& locseg);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Axis_Position().
[[nodiscard]] std::array<double, 3> localNeurosegAxisPositionLegacyLike(const LocalNeuroseg& locseg, double axisOffset);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Axis_Coord_N().
[[nodiscard]] std::array<double, 3> localNeurosegAxisCoordNLegacyLike(const LocalNeuroseg& locseg, double t);

// Port of tz_local_neuroseg.c::Flip_Local_Neuroseg().
void flipLocalNeurosegLegacyLike(LocalNeuroseg* locseg);

// Port of tz_local_neuroseg.c::Next_Local_Neuroseg().
void nextLocalNeurosegLegacyLike(const LocalNeuroseg& locseg1, LocalNeuroseg* locseg2, double posStep);
[[nodiscard]] LocalNeuroseg nextLocalNeurosegLegacyLike(const LocalNeuroseg& locseg1, double posStep);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Scale_Z().
void localNeurosegScaleZLegacyLike(LocalNeuroseg* locseg, double zScale);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Scale().
void localNeurosegScaleLegacyLike(LocalNeuroseg* locseg, double xyScale, double zScale);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Good_Score().
[[nodiscard]] bool localNeurosegGoodScoreLegacyLike(const LocalNeuroseg& locseg, double score, double minScore);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Average_Signal().
[[nodiscard]] double localNeurosegAverageSignalLegacyLike(const LocalNeuroseg& locseg,
                                                          const ZImg& stack,
                                                          double zScale,
                                                          size_t c = 0,
                                                          size_t t = 0);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Top_Sample().
[[nodiscard]] double localNeurosegTopSampleLegacyLike(const LocalNeuroseg& locseg,
                                                      const ZImg& stack,
                                                      double zScale,
                                                      size_t c = 0,
                                                      size_t t = 0);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Height_Profile().
[[nodiscard]] std::vector<double> localNeurosegHeightProfileLegacyLike(const LocalNeuroseg& locseg,
                                                                       const ZImg& stack,
                                                                       double zScale,
                                                                       int n,
                                                                       int option,
                                                                       NeurosegFieldFunctionLegacyLike fieldFunc,
                                                                       size_t c = 0,
                                                                       size_t t = 0);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Height_Search_W().
[[nodiscard]] bool localNeurosegHeightSearchWLegacyLike(LocalNeuroseg* locseg,
                                                        const ZImg& stack,
                                                        double zScale,
                                                        LocsegScoreWorkspace* sws,
                                                        size_t c = 0,
                                                        size_t t = 0);

// Ports of tz_local_neuroseg.c LocalNeuroseg -> Geo3d_Circle helpers.
[[nodiscard]] Geo3dCircle localNeurosegToCircleZLegacyLike(const LocalNeuroseg& locseg, double z, int option);
[[nodiscard]] Geo3dCircle localNeurosegToCircleTLegacyLike(const LocalNeuroseg& locseg, double t, int option);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Hit_Test().
[[nodiscard]] bool localNeurosegHitTestLegacyLike(const LocalNeuroseg& locseg, double x, double y, double z);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Field_S().
[[nodiscard]] Geo3dScalarField localNeurosegFieldSLegacyLike(const LocalNeuroseg& locseg,
                                                             NeurosegFieldFunctionLegacyLike fieldFunc);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Field_Sp().
[[nodiscard]] Geo3dScalarField localNeurosegFieldSpLegacyLike(const LocalNeuroseg& locseg,
                                                              NeurosegFieldFunctionLegacyLike fieldFunc);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Field_Z().
[[nodiscard]] Geo3dScalarField localNeurosegFieldZLegacyLike(const LocalNeuroseg& locseg,
                                                             double z,
                                                             double step,
                                                             NeurosegFieldFunctionLegacyLike fieldFunc);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Score_P().
[[nodiscard]] double localNeurosegScorePLegacyLike(const LocalNeuroseg& locseg,
                                                   const ZImg& stack,
                                                   double zScale,
                                                   StackFitScore* fs,
                                                   size_t c = 0,
                                                   size_t t = 0);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Position_Adjust().
void localNeurosegPositionAdjustLegacyLike(LocalNeuroseg* locseg,
                                           const ZImg& stack,
                                           double zScale,
                                           size_t c = 0,
                                           size_t t = 0);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Orientation_Search_C().
[[nodiscard]] double localNeurosegOrientationSearchCLegacyLike(LocalNeuroseg* locseg,
                                                               const ZImg& stack,
                                                               double zScale,
                                                               StackFitScore* fs,
                                                               size_t c = 0,
                                                               size_t t = 0);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Orientation_Search_B().
[[nodiscard]] double localNeurosegOrientationSearchBLegacyLike(LocalNeuroseg* locseg,
                                                               const ZImg& stack,
                                                               double zScale,
                                                               StackFitScore* fs,
                                                               size_t c = 0,
                                                               size_t t = 0);

// Port of tz_local_neuroseg.c::Local_Neuroseg_R_Scale_Search().
[[nodiscard]] double localNeurosegRScaleSearchLegacyLike(LocalNeuroseg* locseg,
                                                         const ZImg& stack,
                                                         double zScale,
                                                         double rStart,
                                                         double rEnd,
                                                         double rStep,
                                                         double sStart,
                                                         double sEnd,
                                                         double sStep,
                                                         StackFitScore* fs,
                                                         size_t c = 0,
                                                         size_t t = 0);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Score_W().
[[nodiscard]] double localNeurosegScoreWLegacyLike(const LocalNeuroseg& locseg,
                                                   const ZImg& stack,
                                                   double zScale,
                                                   LocsegScoreWorkspace* ws,
                                                   size_t c = 0,
                                                   size_t t = 0);

// Port of tz_local_neuroseg.c::Fit_Local_Neuroseg_W().
[[nodiscard]] double fitLocalNeurosegWLegacyLike(LocalNeuroseg* locseg,
                                                 const ZImg& stack,
                                                 double zScale,
                                                 LocsegFitWorkspace* ws,
                                                 size_t c = 0,
                                                 size_t t = 0);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Optimize_W().
[[nodiscard]] double localNeurosegOptimizeWLegacyLike(LocalNeuroseg* locseg,
                                                      const ZImg& stack,
                                                      double zScale,
                                                      int option,
                                                      LocsegFitWorkspace* ws,
                                                      size_t c = 0,
                                                      size_t t = 0);

} // namespace nim::neutube
