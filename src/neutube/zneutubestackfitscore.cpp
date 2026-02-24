#include "zneutubestackfitscore.h"

#include "zneutubedarraymath.h"
#include "zneutubestackfitoptions.h"

#include "zlog.h"

#include <cmath>

namespace nim::neutube {

namespace {

[[nodiscard]] double
meanSignalWhereFieldNonNegative(const double* fieldValues, const double* signalValues, size_t length)
{
  int n = 0;
  double sum = 0.0;
  for (size_t i = 0; i < length; ++i) {
    if (!std::isnan(signalValues[i]) && fieldValues[i] >= 0.0) {
      sum += signalValues[i];
      ++n;
    }
  }
  return sum / static_cast<double>(n);
}

[[nodiscard]] double meanSignalWhereFieldPositive(const double* fieldValues, const double* signalValues, size_t length)
{
  int n = 0;
  double sum = 0.0;
  for (size_t i = 0; i < length; ++i) {
    if (!std::isnan(signalValues[i]) && fieldValues[i] > 0.0) {
      sum += signalValues[i];
      ++n;
    }
  }
  return sum / static_cast<double>(n);
}

[[nodiscard]] double stackFitStatisticLegacyLike(const double* fieldValues, const double* signalValues, size_t length)
{
  // Port of legacy tz_geo3d_scalar_field.c::field_stack_fit_statistic().
  // Note: darray_mean_n() ignores NaNs in the sum but still divides by length.
  const double mu = darrayMeanNLegacyLike(signalValues, length);

  double var = 0.0;
  for (size_t i = 0; i < length; ++i) {
    if (!std::isnan(signalValues[i])) {
      const double diff = signalValues[i] - mu;
      var += diff * diff;
    }
  }

  if (var == 0.0) {
    return 0.0;
  }

  int length2 = 0;
  const double score = darrayDotNLegacyLike(fieldValues, signalValues, length);
  double filterSum = 0.0;
  double filterSquareSum = 0.0;

  for (size_t i = 0; i < length; ++i) {
    if (!std::isnan(signalValues[i])) {
      filterSum += fieldValues[i];
      filterSquareSum += fieldValues[i] * fieldValues[i];
      ++length2;
    }
  }

  var /= static_cast<double>(length2 - 1);
  return (score - filterSum * mu) / std::sqrt(filterSquareSum * var);
}

[[nodiscard]] double
lowMeanSignalWhereFieldNonNegative(const double* fieldValues, const double* signalValues, size_t length)
{
  int n = 0;
  double mean = 0.0;
  for (size_t i = 0; i < length; ++i) {
    if (!std::isnan(signalValues[i]) && fieldValues[i] >= 0.0) {
      mean += signalValues[i];
      ++n;
    }
  }
  mean /= static_cast<double>(n);

  double sum = 0.0;
  n = 0;
  for (size_t i = 0; i < length; ++i) {
    if (!std::isnan(signalValues[i]) && fieldValues[i] >= 0.0 && signalValues[i] < mean) {
      sum += signalValues[i];
      ++n;
    }
  }

  if (n > static_cast<int>(length / 10)) {
    return sum / static_cast<double>(n);
  }
  return mean;
}

[[nodiscard]] double outerSignalWhereFieldNegative(const double* fieldValues, const double* signalValues, size_t length)
{
  int n = 0;
  double sum = 0.0;
  for (size_t i = 0; i < length; ++i) {
    if (!std::isnan(signalValues[i]) && fieldValues[i] < 0.0) {
      sum += signalValues[i];
      ++n;
    }
  }
  return sum / static_cast<double>(n);
}

[[nodiscard]] double validSignalRatio(const double* signalValues, size_t length)
{
  int count = 0;
  for (size_t i = 0; i < length; ++i) {
    if (!std::isnan(signalValues[i])) {
      ++count;
    }
  }
  return static_cast<double>(count) / static_cast<double>(length);
}

[[nodiscard]] double pdot(const double* fieldValues, const double* signalValues, size_t length)
{
  double score = 0.0;
  for (size_t i = 0; i < length; ++i) {
    if (!std::isnan(signalValues[i]) && fieldValues[i] >= 0.0) {
      score += fieldValues[i] * signalValues[i];
    }
  }
  return score;
}

[[nodiscard]] double pdotPositive(const double* fieldValues, const double* signalValues, size_t length)
{
  double score = 0.0;
  for (size_t i = 0; i < length; ++i) {
    if (!std::isnan(signalValues[i]) && fieldValues[i] > 0.0) {
      score += fieldValues[i] * signalValues[i];
    }
  }
  return score;
}

} // namespace

double
computeStackFitScoresLegacyLike(const double* fieldValues, const double* signalValues, size_t length, StackFitScore* fs)
{
  CHECK(fieldValues != nullptr);
  CHECK(signalValues != nullptr);

  if (fs == nullptr) {
    return darrayDotNLegacyLike(fieldValues, signalValues, length);
  }

  for (int j = 0; j < fs->n; ++j) {
    const auto opt = static_cast<StackFitOption>(fs->options[static_cast<size_t>(j)]);
    switch (opt) {
      case StackFitOption::Dot:
        fs->scores[static_cast<size_t>(j)] = darrayDotNLegacyLike(fieldValues, signalValues, length);
        break;
      case StackFitOption::Corrcoef:
        fs->scores[static_cast<size_t>(j)] = darrayCorrcoefNLegacyLike(fieldValues, signalValues, length);
        break;
      case StackFitOption::Edot:
        fs->scores[static_cast<size_t>(j)] =
          darrayDotNLegacyLike(fieldValues, signalValues, length) + darraySumNLegacyLike(signalValues, length);
        break;
      case StackFitOption::Pdot:
        fs->scores[static_cast<size_t>(j)] = pdot(fieldValues, signalValues, length);
        break;
      case StackFitOption::MeanSignal:
        fs->scores[static_cast<size_t>(j)] = meanSignalWhereFieldNonNegative(fieldValues, signalValues, length);
        break;
      case StackFitOption::LowMeanSignal:
        fs->scores[static_cast<size_t>(j)] = lowMeanSignalWhereFieldNonNegative(fieldValues, signalValues, length);
        break;
      case StackFitOption::CorrcoefSc: {
        const double corr = darrayCorrcoefNLegacyLike(fieldValues, signalValues, length);
        const double maxSignal = darrayMaxLegacyLike(signalValues, length, nullptr);
        fs->scores[static_cast<size_t>(j)] = corr * maxSignal;
        break;
      }
      case StackFitOption::OuterSignal:
        fs->scores[static_cast<size_t>(j)] = outerSignalWhereFieldNegative(fieldValues, signalValues, length);
        break;
      case StackFitOption::ValidSignalRatio:
        fs->scores[static_cast<size_t>(j)] = validSignalRatio(signalValues, length);
        break;
      case StackFitOption::Stat:
        fs->scores[static_cast<size_t>(j)] = stackFitStatisticLegacyLike(fieldValues, signalValues, length);
        break;
      case StackFitOption::DotCenter:
        // Port of STACK_FIT_DOT_CENTER from tz_geo3d_scalar_field.c.
        //
        // Legacy note:
        // The implementation computes centers of `field` and a second field that reuses the
        // exact same point array (only values differ), so the distance is always 0.0 for
        // well-formed fields. Preserve that behavior (dot / (5.0 + 0.0)).
        fs->scores[static_cast<size_t>(j)] = darrayDotNLegacyLike(fieldValues, signalValues, length) / 5.0;
        break;
    }
  }

  return fs->scores[0];
}

double computeStackFitScoresMaskedLegacyLike(const double* fieldValues,
                                             const double* signalValues,
                                             size_t length,
                                             StackFitScore* fs)
{
  CHECK(fieldValues != nullptr);
  CHECK(signalValues != nullptr);

  // Port of tz_geo3d_scalar_field.c::Geo3d_Scalar_Field_Stack_Score_M() scoring switch.
  // Sampling is performed by the caller; this function only computes scores given arrays.
  if (fs == nullptr) {
    return darrayDotNWLegacyLike(fieldValues, signalValues, length);
  }

  for (int j = 0; j < fs->n; ++j) {
    const auto opt = static_cast<StackFitOption>(fs->options[static_cast<size_t>(j)]);
    switch (opt) {
      case StackFitOption::Dot:
        fs->scores[static_cast<size_t>(j)] = darrayDotNLegacyLike(fieldValues, signalValues, length);
        break;
      case StackFitOption::Corrcoef:
        fs->scores[static_cast<size_t>(j)] = darrayCorrcoefNLegacyLike(fieldValues, signalValues, length);
        break;
      case StackFitOption::Edot:
        fs->scores[static_cast<size_t>(j)] =
          darrayDotNLegacyLike(fieldValues, signalValues, length) + darraySumNLegacyLike(signalValues, length);
        break;
      case StackFitOption::Stat:
        fs->scores[static_cast<size_t>(j)] = stackFitStatisticLegacyLike(fieldValues, signalValues, length);
        break;
      case StackFitOption::Pdot:
        // Legacy masked scorer uses strict > 0.0 (not >= 0.0).
        fs->scores[static_cast<size_t>(j)] = pdotPositive(fieldValues, signalValues, length);
        break;
      case StackFitOption::MeanSignal:
        // Legacy masked scorer uses strict > 0.0 (not >= 0.0).
        fs->scores[static_cast<size_t>(j)] = meanSignalWhereFieldPositive(fieldValues, signalValues, length);
        break;
      case StackFitOption::CorrcoefSc: {
        const double corr = darrayCorrcoefNLegacyLike(fieldValues, signalValues, length);
        const double maxSignal = darrayMaxLegacyLike(signalValues, length, nullptr);
        fs->scores[static_cast<size_t>(j)] = corr * maxSignal;
        break;
      }
      case StackFitOption::DotCenter:
        fs->scores[static_cast<size_t>(j)] = darrayDotNLegacyLike(fieldValues, signalValues, length) / 5.0;
        break;
      case StackFitOption::OuterSignal:
        fs->scores[static_cast<size_t>(j)] = outerSignalWhereFieldNegative(fieldValues, signalValues, length);
        break;
      case StackFitOption::ValidSignalRatio:
        fs->scores[static_cast<size_t>(j)] = validSignalRatio(signalValues, length);
        break;
      case StackFitOption::LowMeanSignal:
        // Legacy `Geo3d_Scalar_Field_Stack_Score_M()` does not implement STACK_FIT_LOW_MEAN_SIGNAL.
        CHECK(false) << "STACK_FIT_LOW_MEAN_SIGNAL is not supported by legacy masked stack scorer.";
        break;
    }
  }

  return fs->scores[0];
}

} // namespace nim::neutube
