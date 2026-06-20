#include "zlinearassignment.h"

// The dense rectangular path follows SciPy's modified Jonker-Volgenant
// rectangular LSAP implementation, while the dense/sparse square phases follow
// the Jonker-Volgenant LAPJV/LAPMOD structure used by gatagat/lap.
// See docs/THIRD_PARTY_NOTICES.md for reference licenses.

#include "zexception.h"
#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>

namespace nim {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

[[nodiscard]] bool isMaximize(const ZLinearAssignmentOptions& options)
{
  return options.objective == ZLinearAssignmentObjective::Maximize;
}

[[nodiscard]] std::optional<double> normalizedCostLimit(const ZLinearAssignmentOptions& options)
{
  if (!options.costLimit.has_value()) {
    return std::nullopt;
  }
  const double limit = *options.costLimit;
  if (std::isnan(limit) || limit == -kInf) {
    throw ZException("linear assignment cost_limit must be finite or +inf");
  }
  if (limit == kInf) {
    return std::nullopt;
  }
  if (isMaximize(options)) {
    throw ZException("linear assignment cost_limit is only supported for minimization");
  }
  return limit;
}

void validateAssignmentIndexRange(size_t rows, size_t cols)
{
  constexpr size_t maxIndex = static_cast<size_t>(std::numeric_limits<int32_t>::max());
  if (rows > maxIndex || cols > maxIndex) {
    throw ZException("linear assignment matrix dimensions exceed int32 index range");
  }
}

void validateDenseShapeAndPointer(const double* costs, size_t rows, size_t cols, ptrdiff_t rowStride)
{
  if (rows == 0 || cols == 0) {
    return;
  }
  CHECK(costs);
  if (rowStride < 0 || static_cast<size_t>(rowStride) < cols) {
    throw ZException("linear assignment rowStride must be at least the number of columns");
  }
}

[[nodiscard]] double denseCostAt(const double* costs, size_t row, size_t col, ptrdiff_t rowStride)
{
  return costs[row * static_cast<size_t>(rowStride) + col];
}

[[nodiscard]] bool
validateDenseNumeric(const double* costs, size_t rows, size_t cols, ptrdiff_t rowStride, bool maximize)
{
  bool allFinite = true;
  for (size_t i = 0; i < rows; ++i) {
    const double* rowCosts = costs + i * static_cast<size_t>(rowStride);
    for (size_t j = 0; j < cols; ++j) {
      const double c = rowCosts[j];
      if (std::isnan(c) || c == -kInf) {
        throw ZException("linear assignment cost matrix contains NaN or -inf");
      }
      if (c == kInf) {
        if (maximize) {
          throw ZException("linear assignment +inf costs are not supported for maximization");
        }
        allFinite = false;
      }
    }
  }
  return allFinite;
}

[[nodiscard]] ZLinearAssignmentResult emptyResult(size_t rows, size_t cols)
{
  ZLinearAssignmentResult result;
  result.rowToCol.assign(rows, -1);
  result.colToRow.assign(cols, -1);
  return result;
}

[[nodiscard]] double
assignmentCost(const double* costs, size_t rows, ptrdiff_t rowStride, const std::vector<int32_t>& rowToCol)
{
  double total = 0.0;
  CHECK(rowToCol.size() == rows);
  for (size_t i = 0; i < rows; ++i) {
    const int32_t col = rowToCol[i];
    if (col >= 0) {
      total += denseCostAt(costs, i, static_cast<size_t>(col), rowStride);
    }
  }
  return total;
}

// Dense rectangular shortest augmenting path solver. This follows the
// rectangular Jonker-Volgenant variant used by SciPy/Crouse, with Atlas-owned
// validation and result mapping.
[[nodiscard]] int32_t augmentingPathRect(size_t cols,
                                         const double* costs,
                                         ptrdiff_t rowStride,
                                         std::vector<double>& u,
                                         std::vector<double>& v,
                                         std::vector<int32_t>& path,
                                         std::vector<int32_t>& rowForCol,
                                         std::vector<double>& shortestPathCosts,
                                         size_t startRow,
                                         std::vector<char>& scannedRows,
                                         std::vector<char>& scannedCols,
                                         std::vector<int32_t>& remaining,
                                         double& minValue)
{
  minValue = 0.0;
  size_t numRemaining = cols;
  for (size_t it = 0; it < cols; ++it) {
    remaining[it] = static_cast<int32_t>(cols - it - 1);
  }

  std::fill(scannedRows.begin(), scannedRows.end(), char{0});
  std::fill(scannedCols.begin(), scannedCols.end(), char{0});
  std::fill(shortestPathCosts.begin(), shortestPathCosts.end(), kInf);

  int32_t sink = -1;
  size_t row = startRow;
  while (sink == -1) {
    int32_t index = -1;
    double lowest = kInf;
    scannedRows[row] = char{1};

    for (size_t it = 0; it < numRemaining; ++it) {
      const size_t col = static_cast<size_t>(remaining[it]);
      const double reduced = minValue + denseCostAt(costs, row, col, rowStride) - u[row] - v[col];
      if (reduced < shortestPathCosts[col]) {
        path[col] = static_cast<int32_t>(row);
        shortestPathCosts[col] = reduced;
      }

      if (shortestPathCosts[col] < lowest || (shortestPathCosts[col] == lowest && rowForCol[col] == -1)) {
        lowest = shortestPathCosts[col];
        index = static_cast<int32_t>(it);
      }
    }

    minValue = lowest;
    if (minValue == kInf || index < 0) {
      return -1;
    }

    const size_t col = static_cast<size_t>(remaining[static_cast<size_t>(index)]);
    if (rowForCol[col] == -1) {
      sink = static_cast<int32_t>(col);
    } else {
      row = static_cast<size_t>(rowForCol[col]);
    }
    scannedCols[col] = char{1};
    remaining[static_cast<size_t>(index)] = remaining[--numRemaining];
  }

  return sink;
}

[[nodiscard]] ZLinearAssignmentResult solveRectangularDenseMin(const double* costs,
                                                               size_t rows,
                                                               size_t cols,
                                                               ptrdiff_t rowStride,
                                                               bool transposeResult,
                                                               size_t originalRows,
                                                               size_t originalCols,
                                                               const double* originalCosts,
                                                               ptrdiff_t originalRowStride)
{
  CHECK(rows <= cols);

  std::vector<double> u(rows, 0.0);
  std::vector<double> v(cols, 0.0);
  std::vector<double> shortestPathCosts(cols, 0.0);
  std::vector<int32_t> path(cols, -1);
  std::vector<int32_t> colForRow(rows, -1);
  std::vector<int32_t> rowForCol(cols, -1);
  std::vector<char> scannedRows(rows, char{0});
  std::vector<char> scannedCols(cols, char{0});
  std::vector<int32_t> remaining(cols, -1);

  for (size_t currentRow = 0; currentRow < rows; ++currentRow) {
    double minValue = 0.0;
    int32_t sink = augmentingPathRect(cols,
                                      costs,
                                      rowStride,
                                      u,
                                      v,
                                      path,
                                      rowForCol,
                                      shortestPathCosts,
                                      currentRow,
                                      scannedRows,
                                      scannedCols,
                                      remaining,
                                      minValue);
    if (sink < 0) {
      throw ZException("linear assignment cost matrix is infeasible");
    }

    u[currentRow] += minValue;
    for (size_t i = 0; i < rows; ++i) {
      if (scannedRows[i] && i != currentRow) {
        CHECK(colForRow[i] >= 0);
        u[i] += minValue - shortestPathCosts[static_cast<size_t>(colForRow[i])];
      }
    }
    for (size_t j = 0; j < cols; ++j) {
      if (scannedCols[j]) {
        v[j] -= minValue - shortestPathCosts[j];
      }
    }

    int32_t col = sink;
    while (true) {
      const int32_t row = path[static_cast<size_t>(col)];
      CHECK(row >= 0);
      rowForCol[static_cast<size_t>(col)] = row;
      std::swap(colForRow[static_cast<size_t>(row)], col);
      if (static_cast<size_t>(row) == currentRow) {
        break;
      }
    }
  }

  ZLinearAssignmentResult result;
  result.rowToCol.assign(originalRows, -1);
  result.colToRow.assign(originalCols, -1);

  if (transposeResult) {
    for (size_t originalCol = 0; originalCol < rows; ++originalCol) {
      const int32_t originalRow = colForRow[originalCol];
      CHECK(originalRow >= 0);
      result.rowToCol[static_cast<size_t>(originalRow)] = static_cast<int32_t>(originalCol);
      result.colToRow[originalCol] = originalRow;
    }
  } else {
    for (size_t row = 0; row < rows; ++row) {
      const int32_t col = colForRow[row];
      CHECK(col >= 0);
      result.rowToCol[row] = col;
      result.colToRow[static_cast<size_t>(col)] = static_cast<int32_t>(row);
    }
  }

  result.cost = assignmentCost(originalCosts, originalRows, originalRowStride, result.rowToCol);
  return result;
}

[[nodiscard]] ZLinearAssignmentResult
solveRectangularDense(const double* costs, size_t rows, size_t cols, ptrdiff_t rowStride, bool maximize)
{
  if (rows == 0 || cols == 0) {
    return emptyResult(rows, cols);
  }

  const bool transpose = cols < rows;
  std::vector<double> temp;
  const double* solveCosts = costs;
  ptrdiff_t solveRowStride = rowStride;
  size_t solveRows = rows;
  size_t solveCols = cols;

  if (transpose || maximize) {
    solveRows = transpose ? cols : rows;
    solveCols = transpose ? rows : cols;
    temp.resize(solveRows * solveCols);
    for (size_t i = 0; i < rows; ++i) {
      for (size_t j = 0; j < cols; ++j) {
        double value = denseCostAt(costs, i, j, rowStride);
        if (maximize) {
          value = -value;
        }
        if (transpose) {
          temp[j * solveCols + i] = value;
        } else {
          temp[i * solveCols + j] = value;
        }
      }
    }
    solveCosts = temp.data();
    solveRowStride = static_cast<ptrdiff_t>(solveCols);
  }

  return solveRectangularDenseMin(solveCosts,
                                  solveRows,
                                  solveCols,
                                  solveRowStride,
                                  transpose,
                                  rows,
                                  cols,
                                  costs,
                                  rowStride);
}

[[nodiscard]] int32_t findDenseColumnsWithMinimum(int32_t n, int32_t lo, const double* distances, int32_t* cols)
{
  int32_t hi = lo + 1;
  double minDistance = distances[cols[lo]];
  for (int32_t k = hi; k < n; ++k) {
    const int32_t col = cols[k];
    const double distance = distances[col];
    if (distance <= minDistance) {
      if (distance < minDistance) {
        hi = lo;
        minDistance = distance;
      }
      cols[k] = cols[hi];
      cols[hi++] = col;
    }
  }
  return hi;
}

[[nodiscard]] int32_t scanDenseColumns(int32_t n,
                                       const double* costs,
                                       size_t rowStride,
                                       int32_t& lo,
                                       int32_t& hi,
                                       double* distances,
                                       int32_t* cols,
                                       int32_t* pred,
                                       const int32_t* rowForCol,
                                       const double* colReduction)
{
  int32_t scanLo = lo;
  int32_t scanHi = hi;
  while (scanLo != scanHi) {
    int32_t col = cols[scanLo++];
    const int32_t row = rowForCol[col];
    CHECK(row >= 0);
    const double minDistance = distances[col];
    const double* rowCosts = costs + static_cast<size_t>(row) * rowStride;
    const double h = rowCosts[col] - colReduction[col] - minDistance;

    for (int32_t k = scanHi; k < n; ++k) {
      col = cols[k];
      const double reduced = rowCosts[col] - colReduction[col] - h;
      if (reduced < distances[col]) {
        distances[col] = reduced;
        pred[col] = row;
        if (reduced == minDistance) {
          if (rowForCol[col] < 0) {
            return col;
          }
          cols[k] = cols[scanHi];
          cols[scanHi++] = col;
        }
      }
    }
  }
  lo = scanLo;
  hi = scanHi;
  return -1;
}

[[nodiscard]] int32_t findDenseAugmentingPath(int32_t n,
                                              const double* costs,
                                              size_t rowStride,
                                              int32_t startRow,
                                              const int32_t* rowForCol,
                                              double* colReduction,
                                              int32_t* pred,
                                              int32_t* cols,
                                              double* distances)
{
  int32_t lo = 0;
  int32_t hi = 0;
  int32_t readyCount = 0;
  int32_t finalCol = -1;

  const double* startRowCosts = costs + static_cast<size_t>(startRow) * rowStride;
  for (int32_t col = 0; col < n; ++col) {
    cols[col] = col;
    pred[col] = startRow;
    distances[col] = startRowCosts[col] - colReduction[col];
  }

  while (finalCol == -1) {
    if (lo == hi) {
      readyCount = lo;
      hi = findDenseColumnsWithMinimum(n, lo, distances, cols);
      for (int32_t k = lo; k < hi; ++k) {
        const int32_t col = cols[k];
        if (rowForCol[col] < 0) {
          finalCol = col;
        }
      }
    }
    if (finalCol == -1) {
      finalCol = scanDenseColumns(n, costs, rowStride, lo, hi, distances, cols, pred, rowForCol, colReduction);
    }
  }

  const double minDistance = distances[cols[lo]];
  for (int32_t k = 0; k < readyCount; ++k) {
    const int32_t col = cols[k];
    colReduction[col] += distances[col] - minDistance;
  }

  return finalCol;
}

[[nodiscard]] int32_t denseColumnReductionAndTransfer(int32_t n,
                                                      const double* costs,
                                                      size_t rowStride,
                                                      int32_t* freeRows,
                                                      int32_t* colForRow,
                                                      int32_t* rowForCol,
                                                      double* colReduction)
{
  std::fill(colForRow, colForRow + n, -1);
  std::fill(rowForCol, rowForCol + n, 0);

  std::fill(colReduction, colReduction + n, kInf);
  for (int32_t row = 0; row < n; ++row) {
    const double* rowCosts = costs + static_cast<size_t>(row) * rowStride;
    for (int32_t col = 0; col < n; ++col) {
      const double value = rowCosts[col];
      if (value < colReduction[col]) {
        colReduction[col] = value;
        rowForCol[col] = row;
      }
    }
  }

  std::vector<char> unique(static_cast<size_t>(n), char{1});
  for (int32_t colOffset = n; colOffset > 0; --colOffset) {
    const int32_t col = colOffset - 1;
    const int32_t row = rowForCol[col];
    CHECK(row >= 0);
    if (colForRow[row] < 0) {
      colForRow[row] = col;
    } else {
      unique[row] = char{0};
      rowForCol[col] = -1;
    }
  }

  int32_t numFreeRows = 0;
  for (int32_t row = 0; row < n; ++row) {
    if (colForRow[row] < 0) {
      freeRows[numFreeRows++] = row;
    } else if (unique[row]) {
      const int32_t assignedCol = colForRow[row];
      const double* rowCosts = costs + static_cast<size_t>(row) * rowStride;
      double minReduced = kInf;
      for (int32_t col = 0; col < n; ++col) {
        if (col == assignedCol) {
          continue;
        }
        minReduced = std::min(minReduced, rowCosts[col] - colReduction[col]);
      }
      colReduction[assignedCol] -= minReduced;
    }
  }
  return numFreeRows;
}

[[nodiscard]] int32_t denseAugmentingRowReduction(int32_t n,
                                                  const double* costs,
                                                  size_t rowStride,
                                                  int32_t numFreeRows,
                                                  int32_t* freeRows,
                                                  int32_t* colForRow,
                                                  int32_t* rowForCol,
                                                  double* colReduction)
{
  int32_t current = 0;
  int32_t newFreeRows = 0;
  int32_t reductionCount = 0;
  while (current < numFreeRows) {
    ++reductionCount;
    const int32_t freeRow = freeRows[current++];

    const double* rowCosts = costs + static_cast<size_t>(freeRow) * rowStride;
    double minValue = rowCosts[0] - colReduction[0];
    double secondValue = kInf;
    int32_t bestCol = 0;
    int32_t secondBestCol = 0;
    for (int32_t col = 1; col < n; ++col) {
      const double value = rowCosts[col] - colReduction[col];
      if (value < secondValue) {
        if (value >= minValue) {
          secondValue = value;
          secondBestCol = col;
        } else {
          secondValue = minValue;
          minValue = value;
          secondBestCol = bestCol;
          bestCol = col;
        }
      }
    }

    int32_t displacedRow = rowForCol[bestCol];
    const double newReduction = colReduction[bestCol] - (secondValue - minValue);
    const bool lowersReduction = newReduction < colReduction[bestCol];

    if (reductionCount < current * n) {
      if (lowersReduction) {
        colReduction[bestCol] = newReduction;
      } else if (displacedRow >= 0) {
        bestCol = secondBestCol;
        displacedRow = rowForCol[bestCol];
      }
      if (displacedRow >= 0) {
        if (lowersReduction) {
          freeRows[--current] = displacedRow;
        } else {
          freeRows[newFreeRows++] = displacedRow;
        }
      }
    } else if (displacedRow >= 0) {
      freeRows[newFreeRows++] = displacedRow;
    }

    colForRow[freeRow] = bestCol;
    rowForCol[bestCol] = freeRow;
  }
  return newFreeRows;
}

void denseAugment(int32_t n,
                  const double* costs,
                  size_t rowStride,
                  int32_t numFreeRows,
                  int32_t* freeRows,
                  int32_t* colForRow,
                  int32_t* rowForCol,
                  double* colReduction)
{
  std::vector<int32_t> pred(static_cast<size_t>(n));
  std::vector<int32_t> cols(static_cast<size_t>(n));
  std::vector<double> distances(static_cast<size_t>(n));
  int32_t* predData = pred.data();
  int32_t* colsData = cols.data();
  double* distancesData = distances.data();
  for (int32_t freeRowIndex = 0; freeRowIndex < numFreeRows; ++freeRowIndex) {
    const int32_t freeRow = freeRows[freeRowIndex];
    int32_t row = -1;
    int32_t col =
      findDenseAugmentingPath(n, costs, rowStride, freeRow, rowForCol, colReduction, predData, colsData, distancesData);
    CHECK(col >= 0);

    int32_t guard = 0;
    while (row != freeRow) {
      row = predData[col];
      CHECK(row >= 0);
      rowForCol[col] = row;
      std::swap(col, colForRow[row]);
      CHECK(++guard <= n);
    }
  }
}

[[nodiscard]] ZLinearAssignmentResult solveSquareDenseFastMin(const double* costs, size_t n, ptrdiff_t rowStride)
{
  if (n == 0) {
    return {};
  }
  CHECK(n <= static_cast<size_t>(std::numeric_limits<int32_t>::max()));
  CHECK(rowStride >= 0);
  const int32_t dim = static_cast<int32_t>(n);
  const size_t stride = static_cast<size_t>(rowStride);

  std::vector<int32_t> colForRow(n, -1);
  std::vector<int32_t> rowForCol(n, -1);
  std::vector<int32_t> freeRows(n, -1);
  std::vector<double> colReduction(n, 0.0);

  int32_t* colForRowData = colForRow.data();
  int32_t* rowForColData = rowForCol.data();
  int32_t* freeRowsData = freeRows.data();
  double* colReductionData = colReduction.data();

  int32_t freeRowCount =
    denseColumnReductionAndTransfer(dim, costs, stride, freeRowsData, colForRowData, rowForColData, colReductionData);
  for (int pass = 0; freeRowCount > 0 && pass < 2; ++pass) {
    freeRowCount = denseAugmentingRowReduction(dim,
                                               costs,
                                               stride,
                                               freeRowCount,
                                               freeRowsData,
                                               colForRowData,
                                               rowForColData,
                                               colReductionData);
  }
  if (freeRowCount > 0) {
    denseAugment(dim, costs, stride, freeRowCount, freeRowsData, colForRowData, rowForColData, colReductionData);
  }

  ZLinearAssignmentResult result;
  result.rowToCol = std::move(colForRow);
  result.colToRow = std::move(rowForCol);
  result.cost = assignmentCost(costs, n, rowStride, result.rowToCol);
  return result;
}

[[nodiscard]] ZLinearAssignmentResult
solveSquareDenseFast(const double* costs, size_t n, ptrdiff_t rowStride, bool maximize)
{
  if (!maximize) {
    return solveSquareDenseFastMin(costs, n, rowStride);
  }

  std::vector<double> transformed(n * n);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < n; ++j) {
      transformed[i * n + j] = -denseCostAt(costs, i, j, rowStride);
    }
  }
  ZLinearAssignmentResult result = solveSquareDenseFastMin(transformed.data(), n, static_cast<ptrdiff_t>(n));
  result.cost = assignmentCost(costs, n, rowStride, result.rowToCol);
  return result;
}

[[nodiscard]] ZLinearAssignmentResult
solveWithCostLimit(const double* costs, size_t rows, size_t cols, ptrdiff_t rowStride, double costLimit)
{
  if (rows == 0 || cols == 0) {
    return emptyResult(rows, cols);
  }

  const size_t n = rows + cols;
  std::vector<double> extended(n * n, costLimit / 2.0);
  for (size_t i = rows; i < n; ++i) {
    for (size_t j = cols; j < n; ++j) {
      extended[i * n + j] = 0.0;
    }
  }
  for (size_t i = 0; i < rows; ++i) {
    for (size_t j = 0; j < cols; ++j) {
      extended[i * n + j] = denseCostAt(costs, i, j, rowStride);
    }
  }

  ZLinearAssignmentResult extendedResult =
    solveRectangularDense(extended.data(), n, n, static_cast<ptrdiff_t>(n), false);

  ZLinearAssignmentResult result;
  result.rowToCol.assign(rows, -1);
  result.colToRow.assign(cols, -1);
  for (size_t row = 0; row < rows; ++row) {
    const int32_t col = extendedResult.rowToCol[row];
    if (col >= 0 && static_cast<size_t>(col) < cols) {
      const double c = denseCostAt(costs, row, static_cast<size_t>(col), rowStride);
      if (std::isfinite(c) && c <= costLimit) {
        result.rowToCol[row] = col;
        result.colToRow[static_cast<size_t>(col)] = static_cast<int32_t>(row);
      }
    }
  }
  result.cost = assignmentCost(costs, rows, rowStride, result.rowToCol);
  return result;
}

struct CanonicalCsr
{
  size_t n = 0;
  size_t originalRows = 0;
  size_t originalCols = 0;
  std::span<const int32_t> indptr;
  std::span<const int32_t> indices;
  std::span<const double> costs;
  std::span<const double> originalCosts;
  std::vector<int32_t> ownedIndptr;
  std::vector<int32_t> ownedIndices;
  std::vector<double> ownedCosts;
  std::vector<double> ownedOriginalCosts;
};

void appendCanonicalCsrEdge(CanonicalCsr& csr, size_t col, double cost, bool maximize)
{
  CHECK_LE(col, static_cast<size_t>(std::numeric_limits<int32_t>::max()));
  csr.ownedIndices.push_back(static_cast<int32_t>(col));
  csr.ownedOriginalCosts.push_back(cost);
  csr.ownedCosts.push_back(maximize ? -cost : cost);
}

void bindOwnedCsrStorage(CanonicalCsr& csr)
{
  csr.indptr = std::span<const int32_t>(csr.ownedIndptr.data(), csr.ownedIndptr.size());
  csr.indices = std::span<const int32_t>(csr.ownedIndices.data(), csr.ownedIndices.size());
  csr.costs = std::span<const double>(csr.ownedCosts.data(), csr.ownedCosts.size());
  csr.originalCosts = std::span<const double>(csr.ownedOriginalCosts.data(), csr.ownedOriginalCosts.size());
}

[[nodiscard]] bool validateCsrInput(const ZLinearAssignmentCsrView& view)
{
  if (view.indptr.size() != view.rows + 1) {
    throw ZException("sparse linear assignment indptr length must be rows + 1");
  }
  if (view.indices.size() != view.costs.size()) {
    throw ZException("sparse linear assignment indices and costs lengths differ");
  }
  if (view.indptr[0] != 0) {
    throw ZException("sparse linear assignment indptr must start at 0");
  }

  bool sortedUnique = true;
  const int32_t* const indptr = view.indptr.data();
  const int32_t* const indices = view.indices.data();
  const double* const costs = view.costs.data();
  for (size_t row = 0; row < view.rows; ++row) {
    const int32_t begin = indptr[row];
    const int32_t end = indptr[row + 1];
    if (begin < 0 || end < begin || static_cast<size_t>(end) > view.indices.size()) {
      throw ZException("sparse linear assignment indptr is not monotonic or is out of range");
    }
    int32_t previousCol = -1;
    for (int32_t k = begin; k < end; ++k) {
      const int32_t col = indices[static_cast<size_t>(k)];
      if (col < 0 || static_cast<size_t>(col) >= view.cols) {
        throw ZException("sparse linear assignment column index is out of range");
      }
      const double cost = costs[static_cast<size_t>(k)];
      if (std::isnan(cost) || cost == kInf || cost == -kInf) {
        throw ZException("sparse linear assignment CSR costs must be finite; omit forbidden edges");
      }
      if (col <= previousCol) {
        sortedUnique = false;
      }
      previousCol = col;
    }
  }
  if (view.indptr[view.rows] < 0 || static_cast<size_t>(view.indptr[view.rows]) != view.indices.size()) {
    throw ZException("sparse linear assignment indptr end must equal the number of CSR entries");
  }
  return sortedUnique;
}

[[nodiscard]] CanonicalCsr canonicalizeCsr(const ZLinearAssignmentCsrView& view, bool maximize)
{
  const bool inputSortedUnique = validateCsrInput(view);

  CanonicalCsr csr;
  csr.originalRows = view.rows;
  csr.originalCols = view.cols;
  csr.n = std::max(view.rows, view.cols);
  if (view.rows == 0 || view.cols == 0) {
    return csr;
  }

  if (view.rows == view.cols && inputSortedUnique) {
    csr.indptr = view.indptr;
    csr.indices = view.indices;
    csr.originalCosts = view.costs;
    if (maximize) {
      csr.ownedCosts.reserve(view.costs.size());
      for (const double cost : view.costs) {
        csr.ownedCosts.push_back(-cost);
      }
      csr.costs = std::span<const double>(csr.ownedCosts.data(), csr.ownedCosts.size());
    } else {
      csr.costs = view.costs;
    }
    return csr;
  }

  csr.ownedIndptr.assign(csr.n + 1, 0);
  const size_t extraEdges =
    view.rows > view.cols ? view.rows * (view.rows - view.cols) : (view.cols - view.rows) * view.cols;
  csr.ownedIndices.reserve(view.indices.size() + extraEdges);
  csr.ownedCosts.reserve(view.costs.size() + extraEdges);
  csr.ownedOriginalCosts.reserve(view.costs.size() + extraEdges);

  std::vector<std::pair<int32_t, double>> rowEntries;
  for (size_t row = 0; row < view.rows; ++row) {
    const int32_t begin = view.indptr[row];
    const int32_t end = view.indptr[row + 1];

    csr.ownedIndptr[row] = static_cast<int32_t>(csr.ownedIndices.size());
    if (inputSortedUnique) {
      for (int32_t k = begin; k < end; ++k) {
        appendCanonicalCsrEdge(csr,
                               static_cast<size_t>(view.indices[static_cast<size_t>(k)]),
                               view.costs[static_cast<size_t>(k)],
                               maximize);
      }
    } else {
      rowEntries.clear();
      rowEntries.reserve(static_cast<size_t>(end - begin));
      for (int32_t k = begin; k < end; ++k) {
        rowEntries.emplace_back(view.indices[static_cast<size_t>(k)], view.costs[static_cast<size_t>(k)]);
      }
      std::sort(rowEntries.begin(), rowEntries.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
      });
      for (size_t k = 1; k < rowEntries.size(); ++k) {
        if (rowEntries[k - 1].first == rowEntries[k].first) {
          throw ZException("sparse linear assignment CSR rows must not contain duplicate columns");
        }
      }
      for (const auto& [col, cost] : rowEntries) {
        appendCanonicalCsrEdge(csr, static_cast<size_t>(col), cost, maximize);
      }
    }

    if (view.rows > view.cols) {
      for (size_t col = view.cols; col < csr.n; ++col) {
        appendCanonicalCsrEdge(csr, col, 0.0, false);
      }
    }
  }

  for (size_t row = view.rows; row < csr.n; ++row) {
    csr.ownedIndptr[row] = static_cast<int32_t>(csr.ownedIndices.size());
    for (size_t col = 0; col < csr.n; ++col) {
      appendCanonicalCsrEdge(csr, col, 0.0, false);
    }
  }
  csr.ownedIndptr[csr.n] = static_cast<int32_t>(csr.ownedIndices.size());
  bindOwnedCsrStorage(csr);
  return csr;
}

[[nodiscard]] size_t csrRowBegin(const CanonicalCsr& csr, size_t row)
{
  return static_cast<size_t>(csr.indptr[row]);
}

[[nodiscard]] size_t csrRowEnd(const CanonicalCsr& csr, size_t row)
{
  return static_cast<size_t>(csr.indptr[row + 1]);
}

[[nodiscard]] size_t csrColumn(const CanonicalCsr& csr, size_t entry)
{
  return static_cast<size_t>(csr.indices[entry]);
}

[[nodiscard]] size_t findCsrEntry(const CanonicalCsr& csr, size_t row, int32_t col)
{
  const size_t begin = csrRowBegin(csr, row);
  const size_t end = csrRowEnd(csr, row);
  const auto first = csr.indices.begin() + static_cast<ptrdiff_t>(begin);
  const auto last = csr.indices.begin() + static_cast<ptrdiff_t>(end);
  const auto it = std::lower_bound(first, last, col);
  if (it == last || *it != col) {
    return std::numeric_limits<size_t>::max();
  }
  return static_cast<size_t>(std::distance(csr.indices.begin(), it));
}

[[nodiscard]] size_t sparseReductionTransfer(const CanonicalCsr& csr,
                                             std::vector<int32_t>& freeRows,
                                             std::vector<int32_t>& colForRow,
                                             std::vector<int32_t>& rowForCol,
                                             std::vector<double>& colReduction)
{
  const size_t n = csr.n;
  for (size_t col = 0; col < n; ++col) {
    if (!std::isfinite(colReduction[col])) {
      throw ZException("sparse linear assignment cost matrix is infeasible");
    }
  }

  std::vector<char> unique(n, char{1});
  for (size_t colOffset = n; colOffset > 0; --colOffset) {
    const size_t col = colOffset - 1;
    const int32_t row = rowForCol[col];
    if (colForRow[static_cast<size_t>(row)] < 0) {
      colForRow[static_cast<size_t>(row)] = static_cast<int32_t>(col);
    } else {
      unique[static_cast<size_t>(row)] = char{0};
      rowForCol[col] = -1;
    }
  }

  size_t numFreeRows = 0;
  for (size_t row = 0; row < n; ++row) {
    if (colForRow[row] < 0) {
      freeRows[numFreeRows++] = static_cast<int32_t>(row);
    } else if (unique[row] && csrRowEnd(csr, row) - csrRowBegin(csr, row) > 1) {
      const int32_t assignedCol = colForRow[row];
      double minReduced = kInf;
      for (size_t k = csrRowBegin(csr, row); k < csrRowEnd(csr, row); ++k) {
        const size_t col = csrColumn(csr, k);
        if (col == static_cast<size_t>(assignedCol)) {
          continue;
        }
        minReduced = std::min(minReduced, csr.costs[k] - colReduction[col]);
      }
      if (std::isfinite(minReduced)) {
        colReduction[static_cast<size_t>(assignedCol)] -= minReduced;
      }
    }
  }
  return numFreeRows;
}

[[nodiscard]] size_t sparseColumnReductionAndTransfer(const CanonicalCsr& csr,
                                                      std::vector<int32_t>& freeRows,
                                                      std::vector<int32_t>& colForRow,
                                                      std::vector<int32_t>& rowForCol,
                                                      std::vector<double>& colReduction)
{
  const size_t n = csr.n;
  std::fill(colForRow.begin(), colForRow.end(), -1);
  std::fill(rowForCol.begin(), rowForCol.end(), 0);
  std::fill(colReduction.begin(), colReduction.end(), kInf);

  for (size_t row = 0; row < n; ++row) {
    const size_t rowBegin = csrRowBegin(csr, row);
    const size_t rowEnd = csrRowEnd(csr, row);
    if (rowBegin == rowEnd) {
      throw ZException("sparse linear assignment cost matrix is infeasible");
    }
    for (size_t k = rowBegin; k < rowEnd; ++k) {
      const size_t col = csrColumn(csr, k);
      const double cost = csr.costs[k];
      if (cost < colReduction[col]) {
        colReduction[col] = cost;
        rowForCol[col] = static_cast<int32_t>(row);
      }
    }
  }
  return sparseReductionTransfer(csr, freeRows, colForRow, rowForCol, colReduction);
}

[[nodiscard]] bool sparseSquareColumnReductionAndTransferFromInput(const ZLinearAssignmentCsrView& view,
                                                                   std::vector<int32_t>& freeRows,
                                                                   std::vector<int32_t>& colForRow,
                                                                   std::vector<int32_t>& rowForCol,
                                                                   std::vector<double>& colReduction,
                                                                   size_t& numFreeRows)
{
  CHECK(view.rows == view.cols);
  if (view.indptr.size() != view.rows + 1) {
    throw ZException("sparse linear assignment indptr length must be rows + 1");
  }
  if (view.indices.size() != view.costs.size()) {
    throw ZException("sparse linear assignment indices and costs lengths differ");
  }
  if (view.indptr[0] != 0) {
    throw ZException("sparse linear assignment indptr must start at 0");
  }

  const size_t n = view.rows;
  std::fill(colForRow.begin(), colForRow.end(), -1);
  std::fill(rowForCol.begin(), rowForCol.end(), 0);
  std::fill(colReduction.begin(), colReduction.end(), kInf);

  const int32_t* const indptr = view.indptr.data();
  const int32_t* const indices = view.indices.data();
  const double* const costs = view.costs.data();
  bool sortedUnique = true;
  for (size_t row = 0; row < n; ++row) {
    const int32_t begin = indptr[row];
    const int32_t end = indptr[row + 1];
    if (begin < 0 || end < begin || static_cast<size_t>(end) > view.indices.size()) {
      throw ZException("sparse linear assignment indptr is not monotonic or is out of range");
    }
    if (begin == end) {
      throw ZException("sparse linear assignment cost matrix is infeasible");
    }
    int32_t previousCol = -1;
    for (int32_t k = begin; k < end; ++k) {
      const int32_t col = indices[static_cast<size_t>(k)];
      if (col < 0 || static_cast<size_t>(col) >= view.cols) {
        throw ZException("sparse linear assignment column index is out of range");
      }
      const double cost = costs[static_cast<size_t>(k)];
      if (!std::isfinite(cost)) {
        throw ZException("sparse linear assignment CSR costs must be finite; omit forbidden edges");
      }
      if (col <= previousCol) {
        sortedUnique = false;
      }
      previousCol = col;
      if (cost < colReduction[static_cast<size_t>(col)]) {
        colReduction[static_cast<size_t>(col)] = cost;
        rowForCol[static_cast<size_t>(col)] = static_cast<int32_t>(row);
      }
    }
  }
  if (view.indptr[view.rows] < 0 || static_cast<size_t>(view.indptr[view.rows]) != view.indices.size()) {
    throw ZException("sparse linear assignment indptr end must equal the number of CSR entries");
  }
  if (!sortedUnique) {
    return false;
  }

  CanonicalCsr csr;
  csr.n = n;
  csr.originalRows = view.rows;
  csr.originalCols = view.cols;
  csr.indptr = view.indptr;
  csr.indices = view.indices;
  csr.costs = view.costs;
  csr.originalCosts = view.costs;
  numFreeRows = sparseReductionTransfer(csr, freeRows, colForRow, rowForCol, colReduction);
  return true;
}

[[nodiscard]] size_t sparseAugmentingRowReduction(const CanonicalCsr& csr,
                                                  size_t numFreeRows,
                                                  std::vector<int32_t>& freeRows,
                                                  std::vector<int32_t>& colForRow,
                                                  std::vector<int32_t>& rowForCol,
                                                  std::vector<double>& colReduction)
{
  const int32_t n = static_cast<int32_t>(csr.n);
  const int32_t* const indptr = csr.indptr.data();
  const int32_t* const indices = csr.indices.data();
  const double* const costs = csr.costs.data();
  int32_t* const freeRowsData = freeRows.data();
  int32_t* const colForRowData = colForRow.data();
  int32_t* const rowForColData = rowForCol.data();
  double* const colReductionData = colReduction.data();
  int32_t current = 0;
  int32_t newFreeRows = 0;
  int32_t reductionCount = 0;
  const int32_t totalFreeRows = static_cast<int32_t>(numFreeRows);

  while (current < totalFreeRows) {
    ++reductionCount;
    const int32_t freeRow = freeRowsData[current++];
    const int32_t rowBegin = indptr[freeRow];
    const int32_t rowEnd = indptr[freeRow + 1];

    int32_t bestCol = indices[rowBegin];
    double bestValue = costs[rowBegin] - colReductionData[bestCol];
    int32_t secondBestCol = -1;
    double secondBestValue = kInf;
    for (int32_t k = rowBegin + 1; k < rowEnd; ++k) {
      const int32_t col = indices[k];
      const double value = costs[k] - colReductionData[col];
      if (value < secondBestValue) {
        if (value >= bestValue) {
          secondBestValue = value;
          secondBestCol = col;
        } else {
          secondBestValue = bestValue;
          bestValue = value;
          secondBestCol = bestCol;
          bestCol = col;
        }
      }
    }

    int32_t displacedRow = rowForColData[bestCol];
    if (secondBestCol < 0) {
      if (displacedRow >= 0) {
        freeRowsData[newFreeRows++] = displacedRow;
      }
      colForRowData[freeRow] = bestCol;
      rowForColData[bestCol] = freeRow;
      continue;
    }

    const double newReduction = colReductionData[bestCol] - (secondBestValue - bestValue);
    const bool lowersReduction = newReduction < colReductionData[bestCol];

    if (reductionCount < current * n) {
      if (lowersReduction) {
        colReductionData[bestCol] = newReduction;
      } else if (displacedRow >= 0) {
        bestCol = secondBestCol;
        displacedRow = rowForColData[bestCol];
      }
      if (displacedRow >= 0) {
        if (lowersReduction) {
          freeRowsData[--current] = displacedRow;
        } else {
          freeRowsData[newFreeRows++] = displacedRow;
        }
      }
    } else if (displacedRow >= 0) {
      freeRowsData[newFreeRows++] = displacedRow;
    }

    colForRowData[freeRow] = bestCol;
    rowForColData[bestCol] = freeRow;
  }
  return static_cast<size_t>(newFreeRows);
}

[[nodiscard]] int32_t sparseFindColumnsWithMinimumTodo(const std::vector<double>& distances,
                                                       std::vector<int32_t>& scan,
                                                       size_t todoCount,
                                                       const std::vector<int32_t>& todo,
                                                       const std::vector<char>& done)
{
  int32_t hi = 0;
  double minDistance = kInf;
  for (size_t k = 0; k < todoCount; ++k) {
    const int32_t col = todo[k];
    if (done[static_cast<size_t>(col)]) {
      continue;
    }
    const double distance = distances[static_cast<size_t>(col)];
    if (distance <= minDistance) {
      if (distance < minDistance) {
        hi = 0;
        minDistance = distance;
      }
      scan[static_cast<size_t>(hi++)] = col;
    }
  }
  return hi;
}

[[nodiscard]] int32_t
sparseFindColumnsWithMinimumAll(int32_t n, int32_t lo, const std::vector<double>& distances, std::vector<int32_t>& cols)
{
  const double* const distancesData = distances.data();
  int32_t* const colsData = cols.data();
  int32_t hi = lo + 1;
  double minDistance = distancesData[colsData[lo]];
  for (int32_t k = hi; k < n; ++k) {
    const int32_t col = colsData[k];
    const double distance = distancesData[col];
    if (distance <= minDistance) {
      if (distance < minDistance) {
        hi = lo;
        minDistance = distance;
      }
      colsData[k] = colsData[hi];
      colsData[hi++] = col;
    }
  }
  return hi;
}

[[nodiscard]] int32_t sparseScanAllColumns(const CanonicalCsr& csr,
                                           int32_t& lo,
                                           int32_t& hi,
                                           std::vector<double>& distances,
                                           std::vector<int32_t>& cols,
                                           std::vector<int32_t>& pred,
                                           std::vector<int32_t>& rowEntryByCol,
                                           const std::vector<int32_t>& rowForCol,
                                           const std::vector<double>& colReduction)
{
  const int32_t* const indptr = csr.indptr.data();
  const int32_t* const indices = csr.indices.data();
  const double* const costs = csr.costs.data();
  double* const distancesData = distances.data();
  int32_t* const colsData = cols.data();
  int32_t* const predData = pred.data();
  int32_t* const rowEntryByColData = rowEntryByCol.data();
  const int32_t* const rowForColData = rowForCol.data();
  const double* const colReductionData = colReduction.data();
  int32_t scanLo = lo;
  int32_t scanHi = hi;
  const int32_t n = static_cast<int32_t>(csr.n);
  while (scanLo != scanHi) {
    int32_t col = colsData[scanLo++];
    const int32_t row = rowForColData[col];
    CHECK(row >= 0);
    const double minDistance = distancesData[col];

    std::fill(rowEntryByColData, rowEntryByColData + n, -1);
    const int32_t rowBegin = indptr[row];
    const int32_t rowEnd = indptr[row + 1];
    for (int32_t k = rowBegin; k < rowEnd; ++k) {
      rowEntryByColData[indices[k]] = k;
    }

    const int32_t assignedEntry = rowEntryByColData[col];
    CHECK(assignedEntry >= 0);
    const double h = costs[assignedEntry] - colReductionData[col] - minDistance;

    for (int32_t k = scanHi; k < n; ++k) {
      col = colsData[k];
      const int32_t entry = rowEntryByColData[col];
      if (entry < 0) {
        continue;
      }
      const double reduced = costs[entry] - colReductionData[col] - h;
      if (reduced < distancesData[col]) {
        distancesData[col] = reduced;
        predData[col] = row;
        if (reduced == minDistance) {
          if (rowForColData[col] < 0) {
            return col;
          }
          colsData[k] = colsData[scanHi];
          colsData[scanHi++] = col;
        }
      }
    }
  }
  lo = scanLo;
  hi = scanHi;
  return -1;
}

[[nodiscard]] int32_t sparseFindPathAllColumns(const CanonicalCsr& csr,
                                               int32_t startRow,
                                               const std::vector<int32_t>& rowForCol,
                                               std::vector<double>& colReduction,
                                               std::vector<int32_t>& pred)
{
  const int32_t n = static_cast<int32_t>(csr.n);
  int32_t lo = 0;
  int32_t hi = 0;
  int32_t readyCount = 0;
  int32_t finalCol = -1;
  std::vector<int32_t> cols(csr.n, -1);
  std::vector<int32_t> rowEntryByCol(csr.n, -1);
  std::vector<double> distances(csr.n, kInf);
  int32_t* const colsData = cols.data();
  int32_t* const predData = pred.data();
  double* const distancesData = distances.data();
  const int32_t* const rowForColData = rowForCol.data();
  const double* const colReductionData = colReduction.data();
  const int32_t* const indptr = csr.indptr.data();
  const int32_t* const indices = csr.indices.data();
  const double* const costs = csr.costs.data();

  for (int32_t col = 0; col < n; ++col) {
    colsData[col] = col;
    predData[col] = startRow;
  }

  for (int32_t k = indptr[startRow]; k < indptr[startRow + 1]; ++k) {
    const int32_t col = indices[k];
    distancesData[col] = costs[k] - colReductionData[col];
  }

  while (finalCol == -1) {
    if (lo == hi) {
      readyCount = lo;
      hi = sparseFindColumnsWithMinimumAll(n, lo, distances, cols);
      if (!std::isfinite(distancesData[colsData[lo]])) {
        throw ZException("sparse linear assignment cost matrix is infeasible");
      }
      for (int32_t k = lo; k < hi; ++k) {
        const int32_t col = colsData[k];
        if (rowForColData[col] < 0) {
          finalCol = col;
        }
      }
    }
    if (finalCol == -1) {
      finalCol = sparseScanAllColumns(csr, lo, hi, distances, cols, pred, rowEntryByCol, rowForCol, colReduction);
    }
  }

  const double minDistance = distancesData[colsData[lo]];
  for (int32_t k = 0; k < readyCount; ++k) {
    const int32_t col = colsData[k];
    colReduction[static_cast<size_t>(col)] += distancesData[col] - minDistance;
  }
  return finalCol;
}

[[nodiscard]] int32_t sparseScanTodoColumns(const CanonicalCsr& csr,
                                            size_t& lo,
                                            size_t& hi,
                                            std::vector<double>& distances,
                                            std::vector<int32_t>& pred,
                                            std::vector<char>& done,
                                            size_t& readyCount,
                                            std::vector<int32_t>& ready,
                                            std::vector<int32_t>& scan,
                                            size_t& todoCount,
                                            std::vector<int32_t>& todo,
                                            std::vector<char>& added,
                                            const std::vector<int32_t>& rowForCol,
                                            const std::vector<double>& colReduction)
{
  size_t scanLo = lo;
  size_t scanHi = hi;
  size_t nextReadyCount = readyCount;
  size_t nextTodoCount = todoCount;
  while (scanLo != scanHi) {
    int32_t col = scan[scanLo++];
    const int32_t row = rowForCol[static_cast<size_t>(col)];
    CHECK(row >= 0);
    ready[nextReadyCount++] = col;
    const double minDistance = distances[static_cast<size_t>(col)];

    const size_t assignedEdge = findCsrEntry(csr, static_cast<size_t>(row), col);
    CHECK(assignedEdge != std::numeric_limits<size_t>::max());
    const double h = csr.costs[assignedEdge] - colReduction[static_cast<size_t>(col)] - minDistance;

    for (size_t k = csrRowBegin(csr, static_cast<size_t>(row)); k < csrRowEnd(csr, static_cast<size_t>(row)); ++k) {
      col = static_cast<int32_t>(csrColumn(csr, k));
      if (done[static_cast<size_t>(col)]) {
        continue;
      }
      const double reduced = csr.costs[k] - colReduction[static_cast<size_t>(col)] - h;
      if (reduced < distances[static_cast<size_t>(col)]) {
        distances[static_cast<size_t>(col)] = reduced;
        pred[static_cast<size_t>(col)] = row;
        if (reduced <= minDistance) {
          if (rowForCol[static_cast<size_t>(col)] < 0) {
            return col;
          }
          scan[scanHi++] = col;
          done[static_cast<size_t>(col)] = char{1};
        } else if (!added[static_cast<size_t>(col)]) {
          todo[nextTodoCount++] = col;
          added[static_cast<size_t>(col)] = char{1};
        }
      }
    }
  }
  lo = scanLo;
  hi = scanHi;
  readyCount = nextReadyCount;
  todoCount = nextTodoCount;
  return -1;
}

[[nodiscard]] int32_t sparseFindPathTodoColumns(const CanonicalCsr& csr,
                                                int32_t startRow,
                                                const std::vector<int32_t>& rowForCol,
                                                std::vector<double>& colReduction,
                                                std::vector<int32_t>& pred)
{
  const size_t n = csr.n;
  size_t lo = 0;
  size_t hi = 0;
  int32_t finalCol = -1;
  size_t readyCount = 0;
  size_t todoCount = csrRowEnd(csr, static_cast<size_t>(startRow)) - csrRowBegin(csr, static_cast<size_t>(startRow));

  std::vector<char> done(n, char{0});
  std::vector<char> added(n, char{0});
  std::vector<int32_t> ready(n, -1);
  std::vector<int32_t> scan(n, -1);
  std::vector<int32_t> todo(n, -1);
  std::vector<double> distances(n, kInf);

  for (size_t col = 0; col < n; ++col) {
    pred[col] = startRow;
  }
  const size_t startRowBegin = csrRowBegin(csr, static_cast<size_t>(startRow));
  for (size_t k = startRowBegin; k < csrRowEnd(csr, static_cast<size_t>(startRow)); ++k) {
    const size_t col = csrColumn(csr, k);
    distances[col] = csr.costs[k] - colReduction[col];
    todo[k - startRowBegin] = static_cast<int32_t>(col);
    added[col] = char{1};
  }

  while (finalCol == -1) {
    if (lo == hi) {
      lo = 0;
      const int32_t newHi = sparseFindColumnsWithMinimumTodo(distances, scan, todoCount, todo, done);
      if (newHi <= 0) {
        throw ZException("sparse linear assignment cost matrix is infeasible");
      }
      hi = static_cast<size_t>(newHi);
      for (size_t k = lo; k < hi; ++k) {
        const int32_t col = scan[k];
        if (rowForCol[static_cast<size_t>(col)] < 0) {
          finalCol = col;
        } else {
          done[static_cast<size_t>(col)] = char{1};
        }
      }
    }
    if (finalCol == -1) {
      finalCol = sparseScanTodoColumns(csr,
                                       lo,
                                       hi,
                                       distances,
                                       pred,
                                       done,
                                       readyCount,
                                       ready,
                                       scan,
                                       todoCount,
                                       todo,
                                       added,
                                       rowForCol,
                                       colReduction);
    }
  }

  const double minDistance = distances[static_cast<size_t>(scan[lo])];
  for (size_t k = 0; k < readyCount; ++k) {
    const int32_t col = ready[k];
    colReduction[static_cast<size_t>(col)] += distances[static_cast<size_t>(col)] - minDistance;
  }
  return finalCol;
}

void sparseAugment(const CanonicalCsr& csr,
                   size_t numFreeRows,
                   std::vector<int32_t>& freeRows,
                   std::vector<int32_t>& colForRow,
                   std::vector<int32_t>& rowForCol,
                   std::vector<double>& colReduction)
{
  const bool useAllColumns = csr.costs.size() * 4 > csr.n * csr.n;
  std::vector<int32_t> pred(csr.n, -1);
  for (size_t freeRowIndex = 0; freeRowIndex < numFreeRows; ++freeRowIndex) {
    const int32_t freeRow = freeRows[freeRowIndex];
    int32_t row = -1;
    int32_t col = useAllColumns ? sparseFindPathAllColumns(csr, freeRow, rowForCol, colReduction, pred)
                                : sparseFindPathTodoColumns(csr, freeRow, rowForCol, colReduction, pred);
    CHECK(col >= 0);
    size_t guard = 0;
    while (row != freeRow) {
      row = pred[static_cast<size_t>(col)];
      CHECK(row >= 0);
      rowForCol[static_cast<size_t>(col)] = row;
      std::swap(col, colForRow[static_cast<size_t>(row)]);
      CHECK(++guard <= csr.n);
    }
  }
}

[[nodiscard]] double sparseAssignmentCost(const CanonicalCsr& csr, const std::vector<int32_t>& colForRow)
{
  double total = 0.0;
  CHECK(colForRow.size() == csr.originalRows);
  for (size_t row = 0; row < csr.originalRows; ++row) {
    const int32_t col = colForRow[row];
    if (col < 0) {
      continue;
    }
    const size_t entry = findCsrEntry(csr, row, col);
    CHECK(entry != std::numeric_limits<size_t>::max());
    total += csr.originalCosts[entry];
  }
  return total;
}

} // namespace

std::vector<int32_t> ZLinearAssignmentResult::matchedRows() const
{
  std::vector<int32_t> rows;
  rows.reserve(rowToCol.size());
  for (size_t i = 0; i < rowToCol.size(); ++i) {
    if (rowToCol[i] >= 0) {
      rows.push_back(static_cast<int32_t>(i));
    }
  }
  return rows;
}

std::vector<int32_t> ZLinearAssignmentResult::matchedCols() const
{
  std::vector<int32_t> cols;
  cols.reserve(rowToCol.size());
  for (const int32_t col : rowToCol) {
    if (col >= 0) {
      cols.push_back(col);
    }
  }
  return cols;
}

ZLinearAssignmentResult solveLinearAssignment(const double* costs,
                                              size_t rows,
                                              size_t cols,
                                              ptrdiff_t rowStride,
                                              const ZLinearAssignmentOptions& options)
{
  validateAssignmentIndexRange(rows, cols);
  validateDenseShapeAndPointer(costs, rows, cols, rowStride);
  const auto costLimit = normalizedCostLimit(options);
  const bool maximize = isMaximize(options);
  if (rows == 0 || cols == 0) {
    return emptyResult(rows, cols);
  }

  const bool allFinite = validateDenseNumeric(costs, rows, cols, rowStride, maximize);
  if (costLimit.has_value()) {
    return solveWithCostLimit(costs, rows, cols, rowStride, *costLimit);
  }

  if (rows == cols && allFinite) {
    return solveSquareDenseFast(costs, rows, rowStride, maximize);
  }
  return solveRectangularDense(costs, rows, cols, rowStride, maximize);
}

ZLinearAssignmentResult solveLinearAssignment(std::span<const double> rowMajorCosts,
                                              size_t rows,
                                              size_t cols,
                                              const ZLinearAssignmentOptions& options)
{
  validateAssignmentIndexRange(rows, cols);
  if (rows != 0 && cols > std::numeric_limits<size_t>::max() / rows) {
    throw ZException("linear assignment row-major cost span size overflows rows * cols");
  }
  if (rowMajorCosts.size() != rows * cols) {
    throw ZException("linear assignment row-major cost span size does not match rows * cols");
  }
  return solveLinearAssignment(rowMajorCosts.data(), rows, cols, static_cast<ptrdiff_t>(cols), options);
}

ZLinearAssignmentResult solveLinearAssignment(const Eigen::Ref<const Eigen::MatrixXd>& costs,
                                              const ZLinearAssignmentOptions& options)
{
  std::vector<double> rowMajor(static_cast<size_t>(costs.rows()) * static_cast<size_t>(costs.cols()));
  for (Eigen::Index i = 0; i < costs.rows(); ++i) {
    for (Eigen::Index j = 0; j < costs.cols(); ++j) {
      rowMajor[static_cast<size_t>(i) * static_cast<size_t>(costs.cols()) + static_cast<size_t>(j)] = costs(i, j);
    }
  }
  return solveLinearAssignment(rowMajor, static_cast<size_t>(costs.rows()), static_cast<size_t>(costs.cols()), options);
}

ZLinearAssignmentResult solveLinearAssignmentCsr(const ZLinearAssignmentCsrView& view,
                                                 const ZLinearAssignmentOptions& options)
{
  validateAssignmentIndexRange(view.rows, view.cols);
  const auto costLimit = normalizedCostLimit(options);
  if (costLimit.has_value()) {
    throw ZException("sparse linear assignment does not support cost_limit");
  }

  const bool maximize = isMaximize(options);
  if (view.rows == 0 || view.cols == 0) {
    return emptyResult(view.rows, view.cols);
  }

  CanonicalCsr csr;
  std::vector<int32_t> colForRow;
  std::vector<int32_t> rowForCol;
  std::vector<int32_t> freeRows;
  std::vector<double> colReduction;
  size_t numFreeRows = 0;
  bool initializedReduction = false;
  if (!maximize && view.rows == view.cols) {
    colForRow.assign(view.rows, -1);
    rowForCol.assign(view.rows, -1);
    freeRows.assign(view.rows, -1);
    colReduction.assign(view.rows, 0.0);
    initializedReduction =
      sparseSquareColumnReductionAndTransferFromInput(view, freeRows, colForRow, rowForCol, colReduction, numFreeRows);
    if (initializedReduction) {
      csr.n = view.rows;
      csr.originalRows = view.rows;
      csr.originalCols = view.cols;
      csr.indptr = view.indptr;
      csr.indices = view.indices;
      csr.costs = view.costs;
      csr.originalCosts = view.costs;
    }
  }
  if (!initializedReduction) {
    csr = canonicalizeCsr(view, maximize);
    if (csr.n == 0) {
      return emptyResult(view.rows, view.cols);
    }
    colForRow.assign(csr.n, -1);
    rowForCol.assign(csr.n, -1);
    freeRows.assign(csr.n, -1);
    colReduction.assign(csr.n, 0.0);
    numFreeRows = sparseColumnReductionAndTransfer(csr, freeRows, colForRow, rowForCol, colReduction);
  }
  for (int pass = 0; numFreeRows > 0 && pass < 2; ++pass) {
    numFreeRows = sparseAugmentingRowReduction(csr, numFreeRows, freeRows, colForRow, rowForCol, colReduction);
  }
  if (numFreeRows > 0) {
    sparseAugment(csr, numFreeRows, freeRows, colForRow, rowForCol, colReduction);
  }

  ZLinearAssignmentResult result;
  result.rowToCol.assign(csr.originalRows, -1);
  result.colToRow.assign(csr.originalCols, -1);
  for (size_t row = 0; row < csr.originalRows; ++row) {
    const int32_t col = colForRow[row];
    if (col >= 0 && static_cast<size_t>(col) < csr.originalCols) {
      result.rowToCol[row] = col;
      result.colToRow[static_cast<size_t>(col)] = static_cast<int32_t>(row);
    }
  }
  result.cost = sparseAssignmentCost(csr, result.rowToCol);
  return result;
}

} // namespace nim
