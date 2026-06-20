#pragma once

#include "zexception.h"
#include "zeigenutils.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace nim {

enum class ZLinearAssignmentObjective
{
  Minimize,
  Maximize,
};

struct ZLinearAssignmentOptions
{
  ZLinearAssignmentObjective objective = ZLinearAssignmentObjective::Minimize;
  std::optional<double> costLimit;
};

struct ZLinearAssignmentResult
{
  double cost = 0.0;
  std::vector<int32_t> rowToCol;
  std::vector<int32_t> colToRow;

  [[nodiscard]] std::vector<int32_t> matchedRows() const;

  [[nodiscard]] std::vector<int32_t> matchedCols() const;
};

struct ZLinearAssignmentCsrView
{
  size_t rows = 0;
  size_t cols = 0;
  std::span<const int32_t> indptr;
  std::span<const int32_t> indices;
  std::span<const double> costs;
};

namespace detail {

template<typename Cost>
using ZLinearAssignmentCostScalar = std::remove_cv_t<Cost>;

template<typename Cost>
inline constexpr bool kZLinearAssignmentCost =
  std::is_arithmetic_v<ZLinearAssignmentCostScalar<Cost>> && !std::is_same_v<ZLinearAssignmentCostScalar<Cost>, bool>;

template<typename Cost>
inline constexpr bool kZLinearAssignmentConvertingCost =
  kZLinearAssignmentCost<Cost> && !std::is_same_v<ZLinearAssignmentCostScalar<Cost>, double>;

template<typename Index>
using ZLinearAssignmentIndexScalar = std::remove_cv_t<Index>;

template<typename Index>
inline constexpr bool kZLinearAssignmentIndex =
  std::is_integral_v<ZLinearAssignmentIndexScalar<Index>> && !std::is_same_v<ZLinearAssignmentIndexScalar<Index>, bool>;

template<typename Index>
inline constexpr bool kZLinearAssignmentConvertingIndex =
  kZLinearAssignmentIndex<Index> && !std::is_same_v<ZLinearAssignmentIndexScalar<Index>, int32_t>;

template<typename Cost>
[[nodiscard]] std::vector<double> copyLinearAssignmentCostsToDouble(std::span<const Cost> costs)
{
  std::vector<double> converted;
  converted.reserve(costs.size());
  for (const Cost cost : costs) {
    converted.push_back(static_cast<double>(cost));
  }
  return converted;
}

template<typename Index>
[[nodiscard]] std::vector<int32_t> copyLinearAssignmentIndicesToInt32(std::span<const Index> indices, const char* name)
{
  std::vector<int32_t> converted;
  converted.reserve(indices.size());
  constexpr int32_t maxIndex = std::numeric_limits<int32_t>::max();
  for (const Index index : indices) {
    bool inRange = false;
    if constexpr (std::is_signed_v<ZLinearAssignmentIndexScalar<Index>>) {
      const int64_t widened = static_cast<int64_t>(index);
      inRange = widened >= 0 && widened <= static_cast<int64_t>(maxIndex);
    } else {
      const uint64_t widened = static_cast<uint64_t>(index);
      inRange = widened <= static_cast<uint64_t>(maxIndex);
    }
    if (!inRange) {
      throw ZException(std::string("linear assignment CSR ") + name + " value is outside int32 range");
    }
    converted.push_back(static_cast<int32_t>(index));
  }
  return converted;
}

} // namespace detail

[[nodiscard]] ZLinearAssignmentResult solveLinearAssignment(const double* costs,
                                                            size_t rows,
                                                            size_t cols,
                                                            ptrdiff_t rowStride,
                                                            const ZLinearAssignmentOptions& options = {});

[[nodiscard]] ZLinearAssignmentResult solveLinearAssignment(std::span<const double> rowMajorCosts,
                                                            size_t rows,
                                                            size_t cols,
                                                            const ZLinearAssignmentOptions& options = {});

template<typename Cost>
  requires detail::kZLinearAssignmentConvertingCost<Cost>
[[nodiscard]] ZLinearAssignmentResult solveLinearAssignment(std::span<const Cost> rowMajorCosts,
                                                            size_t rows,
                                                            size_t cols,
                                                            const ZLinearAssignmentOptions& options = {})
{
  const std::vector<double> converted = detail::copyLinearAssignmentCostsToDouble(rowMajorCosts);
  return solveLinearAssignment(std::span<const double>(converted.data(), converted.size()), rows, cols, options);
}

template<typename Cost>
  requires detail::kZLinearAssignmentConvertingCost<Cost>
[[nodiscard]] ZLinearAssignmentResult solveLinearAssignment(const std::vector<Cost>& rowMajorCosts,
                                                            size_t rows,
                                                            size_t cols,
                                                            const ZLinearAssignmentOptions& options = {})
{
  return solveLinearAssignment(std::span<const Cost>(rowMajorCosts.data(), rowMajorCosts.size()), rows, cols, options);
}

[[nodiscard]] ZLinearAssignmentResult solveLinearAssignment(const Eigen::Ref<const Eigen::MatrixXd>& costs,
                                                            const ZLinearAssignmentOptions& options = {});

[[nodiscard]] ZLinearAssignmentResult solveLinearAssignmentCsr(const ZLinearAssignmentCsrView& costs,
                                                               const ZLinearAssignmentOptions& options = {});

[[nodiscard]] inline ZLinearAssignmentResult solveLinearAssignmentCsr(size_t rows,
                                                                      size_t cols,
                                                                      std::span<const int32_t> indptr,
                                                                      std::span<const int32_t> indices,
                                                                      std::span<const double> costs,
                                                                      const ZLinearAssignmentOptions& options = {})
{
  return solveLinearAssignmentCsr(ZLinearAssignmentCsrView{rows, cols, indptr, indices, costs}, options);
}

template<typename IndptrIndex, typename ColumnIndex, typename Cost>
  requires detail::kZLinearAssignmentIndex<IndptrIndex> && detail::kZLinearAssignmentIndex<ColumnIndex> &&
           detail::kZLinearAssignmentCost<Cost> &&
           (detail::kZLinearAssignmentConvertingIndex<IndptrIndex> ||
            detail::kZLinearAssignmentConvertingIndex<ColumnIndex> || detail::kZLinearAssignmentConvertingCost<Cost>)
[[nodiscard]] ZLinearAssignmentResult solveLinearAssignmentCsr(size_t rows,
                                                               size_t cols,
                                                               std::span<const IndptrIndex> indptr,
                                                               std::span<const ColumnIndex> indices,
                                                               std::span<const Cost> costs,
                                                               const ZLinearAssignmentOptions& options = {})
{
  std::vector<int32_t> convertedIndptr;
  std::vector<int32_t> convertedIndices;
  std::vector<double> convertedCosts;

  std::span<const int32_t> indptrView;
  if constexpr (detail::kZLinearAssignmentConvertingIndex<IndptrIndex>) {
    convertedIndptr = detail::copyLinearAssignmentIndicesToInt32(indptr, "indptr");
    indptrView = std::span<const int32_t>(convertedIndptr.data(), convertedIndptr.size());
  } else {
    indptrView = indptr;
  }

  std::span<const int32_t> indicesView;
  if constexpr (detail::kZLinearAssignmentConvertingIndex<ColumnIndex>) {
    convertedIndices = detail::copyLinearAssignmentIndicesToInt32(indices, "indices");
    indicesView = std::span<const int32_t>(convertedIndices.data(), convertedIndices.size());
  } else {
    indicesView = indices;
  }

  std::span<const double> costsView;
  if constexpr (detail::kZLinearAssignmentConvertingCost<Cost>) {
    convertedCosts = detail::copyLinearAssignmentCostsToDouble(costs);
    costsView = std::span<const double>(convertedCosts.data(), convertedCosts.size());
  } else {
    costsView = costs;
  }

  return solveLinearAssignmentCsr(rows, cols, indptrView, indicesView, costsView, options);
}

template<typename IndptrIndex, typename ColumnIndex, typename Cost>
  requires detail::kZLinearAssignmentIndex<IndptrIndex> && detail::kZLinearAssignmentIndex<ColumnIndex> &&
           detail::kZLinearAssignmentCost<Cost> &&
           (detail::kZLinearAssignmentConvertingIndex<IndptrIndex> ||
            detail::kZLinearAssignmentConvertingIndex<ColumnIndex> || detail::kZLinearAssignmentConvertingCost<Cost>)
[[nodiscard]] ZLinearAssignmentResult solveLinearAssignmentCsr(size_t rows,
                                                               size_t cols,
                                                               const std::vector<IndptrIndex>& indptr,
                                                               const std::vector<ColumnIndex>& indices,
                                                               const std::vector<Cost>& costs,
                                                               const ZLinearAssignmentOptions& options = {})
{
  return solveLinearAssignmentCsr(rows,
                                  cols,
                                  std::span<const IndptrIndex>(indptr.data(), indptr.size()),
                                  std::span<const ColumnIndex>(indices.data(), indices.size()),
                                  std::span<const Cost>(costs.data(), costs.size()),
                                  options);
}

} // namespace nim
