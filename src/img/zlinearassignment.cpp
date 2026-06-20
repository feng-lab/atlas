#include "zlinearassignment.h"

// The dense rectangular path follows SciPy's modified Jonker-Volgenant
// rectangular LSAP implementation, while the dense/sparse square phases follow
// the Jonker-Volgenant LAPJV/LAPMOD structure used by gatagat/lap.
// See docs/THIRD_PARTY_NOTICES.md for reference licenses.

#include "zexception.h"
#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <functional>
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
  std::vector<size_t> indptr;
  std::vector<size_t> indices;
  std::vector<double> costs;
  std::vector<double> originalCosts;
};

[[nodiscard]] CanonicalCsr canonicalizeCsr(const ZLinearAssignmentCsrView& view, bool maximize)
{
  if (view.rows != view.cols) {
    throw ZException("sparse linear assignment currently requires a square CSR matrix");
  }
  if (view.indptr.size() != view.rows + 1) {
    throw ZException("sparse linear assignment indptr length must be rows + 1");
  }
  if (view.indices.size() != view.costs.size()) {
    throw ZException("sparse linear assignment indices and costs lengths differ");
  }
  if (view.rows == 0) {
    return {};
  }
  if (view.indptr[0] != 0) {
    throw ZException("sparse linear assignment indptr must start at 0");
  }

  CanonicalCsr csr;
  csr.n = view.rows;
  csr.indptr.assign(csr.n + 1, 0);

  std::vector<std::pair<int32_t, double>> rowEntries;
  for (size_t row = 0; row < csr.n; ++row) {
    const int32_t begin = view.indptr[row];
    const int32_t end = view.indptr[row + 1];
    if (begin < 0 || end < begin || static_cast<size_t>(end) > view.indices.size()) {
      throw ZException("sparse linear assignment indptr is not monotonic or is out of range");
    }
    rowEntries.clear();
    rowEntries.reserve(static_cast<size_t>(end - begin));
    for (int32_t k = begin; k < end; ++k) {
      const int32_t col = view.indices[static_cast<size_t>(k)];
      if (col < 0 || static_cast<size_t>(col) >= view.cols) {
        throw ZException("sparse linear assignment column index is out of range");
      }
      const double cost = view.costs[static_cast<size_t>(k)];
      if (!std::isfinite(cost)) {
        throw ZException("sparse linear assignment CSR costs must be finite; omit forbidden edges");
      }
      rowEntries.emplace_back(col, cost);
    }
    std::sort(rowEntries.begin(), rowEntries.end(), [](const auto& a, const auto& b) {
      return a.first < b.first;
    });
    for (size_t k = 1; k < rowEntries.size(); ++k) {
      if (rowEntries[k - 1].first == rowEntries[k].first) {
        throw ZException("sparse linear assignment CSR rows must not contain duplicate columns");
      }
    }

    csr.indptr[row] = csr.indices.size();
    for (const auto& [col, cost] : rowEntries) {
      csr.indices.push_back(static_cast<size_t>(col));
      csr.originalCosts.push_back(cost);
      csr.costs.push_back(maximize ? -cost : cost);
    }
  }
  csr.indptr[csr.n] = csr.indices.size();
  return csr;
}

[[nodiscard]] bool csrHasPerfectMatching(const CanonicalCsr& csr)
{
  std::vector<int32_t> rowForCol(csr.n, -1);
  std::vector<char> seen(csr.n, char{0});

  std::function<bool(size_t)> visit = [&](size_t row) {
    for (size_t k = csr.indptr[row]; k < csr.indptr[row + 1]; ++k) {
      const size_t col = csr.indices[k];
      if (seen[col]) {
        continue;
      }
      seen[col] = char{1};
      if (rowForCol[col] < 0 || visit(static_cast<size_t>(rowForCol[col]))) {
        rowForCol[col] = static_cast<int32_t>(row);
        return true;
      }
    }
    return false;
  };

  for (size_t row = 0; row < csr.n; ++row) {
    std::fill(seen.begin(), seen.end(), char{0});
    if (!visit(row)) {
      return false;
    }
  }
  return true;
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
    for (size_t k = csr.indptr[row]; k < csr.indptr[row + 1]; ++k) {
      const size_t col = csr.indices[k];
      const double cost = csr.costs[k];
      if (cost < colReduction[col]) {
        colReduction[col] = cost;
        rowForCol[col] = static_cast<int32_t>(row);
      }
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
    } else if (unique[row] && csr.indptr[row + 1] - csr.indptr[row] > 1) {
      const int32_t assignedCol = colForRow[row];
      double minReduced = kInf;
      for (size_t k = csr.indptr[row]; k < csr.indptr[row + 1]; ++k) {
        const size_t col = csr.indices[k];
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

[[nodiscard]] size_t sparseAugmentingRowReduction(const CanonicalCsr& csr,
                                                  size_t numFreeRows,
                                                  std::vector<int32_t>& freeRows,
                                                  std::vector<int32_t>& colForRow,
                                                  std::vector<int32_t>& rowForCol,
                                                  std::vector<double>& colReduction)
{
  const size_t n = csr.n;
  size_t current = 0;
  size_t newFreeRows = 0;
  size_t reductionCount = 0;

  while (current < numFreeRows) {
    ++reductionCount;
    const int32_t freeRow = freeRows[current++];
    const size_t rowBegin = csr.indptr[static_cast<size_t>(freeRow)];
    const size_t rowEnd = csr.indptr[static_cast<size_t>(freeRow) + 1];
    CHECK(rowBegin < rowEnd);

    size_t bestCol = csr.indices[rowBegin];
    double bestValue = csr.costs[rowBegin] - colReduction[bestCol];
    size_t secondBestCol = bestCol;
    double secondBestValue = kInf;
    for (size_t k = rowBegin + 1; k < rowEnd; ++k) {
      const size_t col = csr.indices[k];
      const double value = csr.costs[k] - colReduction[col];
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

    int32_t displacedRow = rowForCol[bestCol];
    const bool hasSecond = std::isfinite(secondBestValue) && secondBestCol != bestCol;
    const double newReduction =
      hasSecond ? colReduction[bestCol] - (secondBestValue - bestValue) : colReduction[bestCol];
    const bool lowersReduction = hasSecond && newReduction < colReduction[bestCol];

    if (reductionCount < current * n) {
      if (lowersReduction) {
        colReduction[bestCol] = newReduction;
      } else if (displacedRow >= 0 && hasSecond) {
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

    colForRow[static_cast<size_t>(freeRow)] = static_cast<int32_t>(bestCol);
    rowForCol[bestCol] = freeRow;
  }
  return newFreeRows;
}

[[nodiscard]] size_t
sparseFindColumnsWithMinimum(size_t n, size_t lo, const std::vector<double>& distances, std::vector<int32_t>& cols)
{
  size_t hi = lo + 1;
  double minDistance = distances[static_cast<size_t>(cols[lo])];
  for (size_t k = hi; k < n; ++k) {
    const int32_t col = cols[k];
    const double distance = distances[static_cast<size_t>(col)];
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

[[nodiscard]] int32_t sparseScanAllColumns(const CanonicalCsr& csr,
                                           size_t& lo,
                                           size_t& hi,
                                           std::vector<double>& distances,
                                           std::vector<int32_t>& cols,
                                           std::vector<int32_t>& pred,
                                           const std::vector<int32_t>& rowForCol,
                                           const std::vector<double>& colReduction)
{
  std::vector<int32_t> reverseCol(csr.n, -1);
  while (lo != hi) {
    int32_t col = cols[lo++];
    const int32_t row = rowForCol[static_cast<size_t>(col)];
    CHECK(row >= 0);
    const double minDistance = distances[static_cast<size_t>(col)];

    for (size_t k = csr.indptr[static_cast<size_t>(row)]; k < csr.indptr[static_cast<size_t>(row) + 1]; ++k) {
      reverseCol[csr.indices[k]] = static_cast<int32_t>(k);
    }

    const int32_t assignedEdge = reverseCol[static_cast<size_t>(col)];
    if (assignedEdge >= 0) {
      const double h =
        csr.costs[static_cast<size_t>(assignedEdge)] - colReduction[static_cast<size_t>(col)] - minDistance;
      for (size_t k = hi; k < csr.n; ++k) {
        col = cols[k];
        const int32_t edge = reverseCol[static_cast<size_t>(col)];
        if (edge < 0) {
          continue;
        }
        const double reduced = csr.costs[static_cast<size_t>(edge)] - colReduction[static_cast<size_t>(col)] - h;
        if (reduced < distances[static_cast<size_t>(col)]) {
          distances[static_cast<size_t>(col)] = reduced;
          pred[static_cast<size_t>(col)] = row;
          if (reduced == minDistance) {
            if (rowForCol[static_cast<size_t>(col)] < 0) {
              for (size_t rk = csr.indptr[static_cast<size_t>(row)]; rk < csr.indptr[static_cast<size_t>(row) + 1];
                   ++rk) {
                reverseCol[csr.indices[rk]] = -1;
              }
              return col;
            }
            cols[k] = cols[hi];
            cols[hi++] = col;
          }
        }
      }
    }

    for (size_t k = csr.indptr[static_cast<size_t>(row)]; k < csr.indptr[static_cast<size_t>(row) + 1]; ++k) {
      reverseCol[csr.indices[k]] = -1;
    }
  }
  return -1;
}

[[nodiscard]] int32_t sparseFindPathAllColumns(const CanonicalCsr& csr,
                                               int32_t startRow,
                                               const std::vector<int32_t>& rowForCol,
                                               std::vector<double>& colReduction,
                                               std::vector<int32_t>& pred)
{
  const size_t n = csr.n;
  size_t lo = 0;
  size_t hi = 0;
  size_t readyCount = 0;
  int32_t finalCol = -1;
  std::vector<int32_t> cols(n, -1);
  std::vector<double> distances(n, kInf);

  for (size_t col = 0; col < n; ++col) {
    cols[col] = static_cast<int32_t>(col);
    pred[col] = startRow;
  }
  for (size_t k = csr.indptr[static_cast<size_t>(startRow)]; k < csr.indptr[static_cast<size_t>(startRow) + 1]; ++k) {
    const size_t col = csr.indices[k];
    distances[col] = csr.costs[k] - colReduction[col];
  }

  while (finalCol == -1) {
    if (lo == hi) {
      readyCount = lo;
      if (lo >= n) {
        throw ZException("sparse linear assignment internal search failed");
      }
      hi = sparseFindColumnsWithMinimum(n, lo, distances, cols);
      if (distances[static_cast<size_t>(cols[lo])] == kInf) {
        throw ZException("sparse linear assignment cost matrix is infeasible");
      }
      for (size_t k = lo; k < hi; ++k) {
        const int32_t col = cols[k];
        if (rowForCol[static_cast<size_t>(col)] < 0) {
          finalCol = col;
        }
      }
    }
    if (finalCol == -1) {
      finalCol = sparseScanAllColumns(csr, lo, hi, distances, cols, pred, rowForCol, colReduction);
    }
  }

  const double minDistance = distances[static_cast<size_t>(cols[lo])];
  for (size_t k = 0; k < readyCount; ++k) {
    const int32_t col = cols[k];
    colReduction[static_cast<size_t>(col)] += distances[static_cast<size_t>(col)] - minDistance;
  }
  return finalCol;
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
  std::vector<int32_t> reverseCol(csr.n, -1);
  while (lo != hi) {
    int32_t col = scan[lo++];
    const int32_t row = rowForCol[static_cast<size_t>(col)];
    CHECK(row >= 0);
    ready[readyCount++] = col;
    const double minDistance = distances[static_cast<size_t>(col)];

    for (size_t k = csr.indptr[static_cast<size_t>(row)]; k < csr.indptr[static_cast<size_t>(row) + 1]; ++k) {
      reverseCol[csr.indices[k]] = static_cast<int32_t>(k);
    }

    const int32_t assignedEdge = reverseCol[static_cast<size_t>(col)];
    CHECK(assignedEdge >= 0);
    const double h =
      csr.costs[static_cast<size_t>(assignedEdge)] - colReduction[static_cast<size_t>(col)] - minDistance;

    for (size_t k = csr.indptr[static_cast<size_t>(row)]; k < csr.indptr[static_cast<size_t>(row) + 1]; ++k) {
      col = static_cast<int32_t>(csr.indices[k]);
      if (done[static_cast<size_t>(col)]) {
        continue;
      }
      const double reduced = csr.costs[k] - colReduction[static_cast<size_t>(col)] - h;
      if (reduced < distances[static_cast<size_t>(col)]) {
        distances[static_cast<size_t>(col)] = reduced;
        pred[static_cast<size_t>(col)] = row;
        if (reduced <= minDistance) {
          if (rowForCol[static_cast<size_t>(col)] < 0) {
            for (size_t rk = csr.indptr[static_cast<size_t>(row)]; rk < csr.indptr[static_cast<size_t>(row) + 1];
                 ++rk) {
              reverseCol[csr.indices[rk]] = -1;
            }
            return col;
          }
          scan[hi++] = col;
          done[static_cast<size_t>(col)] = char{1};
        } else if (!added[static_cast<size_t>(col)]) {
          todo[todoCount++] = col;
          added[static_cast<size_t>(col)] = char{1};
        }
      }
    }

    for (size_t k = csr.indptr[static_cast<size_t>(row)]; k < csr.indptr[static_cast<size_t>(row) + 1]; ++k) {
      reverseCol[csr.indices[k]] = -1;
    }
  }
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
  size_t todoCount = csr.indptr[static_cast<size_t>(startRow) + 1] - csr.indptr[static_cast<size_t>(startRow)];

  std::vector<char> done(n, char{0});
  std::vector<char> added(n, char{0});
  std::vector<int32_t> ready(n, -1);
  std::vector<int32_t> scan(n, -1);
  std::vector<int32_t> todo(n, -1);
  std::vector<double> distances(n, kInf);

  for (size_t col = 0; col < n; ++col) {
    pred[col] = startRow;
  }
  for (size_t k = csr.indptr[static_cast<size_t>(startRow)]; k < csr.indptr[static_cast<size_t>(startRow) + 1]; ++k) {
    const size_t col = csr.indices[k];
    distances[col] = csr.costs[k] - colReduction[col];
    todo[k - csr.indptr[static_cast<size_t>(startRow)]] = static_cast<int32_t>(col);
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

[[nodiscard]] int32_t sparseFindPathDynamic(const CanonicalCsr& csr,
                                            int32_t startRow,
                                            const std::vector<int32_t>& rowForCol,
                                            std::vector<double>& colReduction,
                                            std::vector<int32_t>& pred)
{
  const double sparsity = static_cast<double>(csr.costs.size()) / static_cast<double>(csr.n * csr.n);
  if (sparsity > 0.25) {
    return sparseFindPathAllColumns(csr, startRow, rowForCol, colReduction, pred);
  }
  return sparseFindPathTodoColumns(csr, startRow, rowForCol, colReduction, pred);
}

void sparseAugment(const CanonicalCsr& csr,
                   size_t numFreeRows,
                   std::vector<int32_t>& freeRows,
                   std::vector<int32_t>& colForRow,
                   std::vector<int32_t>& rowForCol,
                   std::vector<double>& colReduction)
{
  std::vector<int32_t> pred(csr.n, -1);
  for (size_t freeRowIndex = 0; freeRowIndex < numFreeRows; ++freeRowIndex) {
    const int32_t freeRow = freeRows[freeRowIndex];
    int32_t row = -1;
    int32_t col = sparseFindPathDynamic(csr, freeRow, rowForCol, colReduction, pred);
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
  for (size_t row = 0; row < csr.n; ++row) {
    const int32_t col = colForRow[row];
    CHECK(col >= 0);
    bool found = false;
    for (size_t k = csr.indptr[row]; k < csr.indptr[row + 1]; ++k) {
      if (csr.indices[k] == static_cast<size_t>(col)) {
        total += csr.originalCosts[k];
        found = true;
        break;
      }
    }
    CHECK(found);
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

  CanonicalCsr csr = canonicalizeCsr(view, isMaximize(options));
  if (csr.n == 0) {
    return {};
  }
  if (!csrHasPerfectMatching(csr)) {
    throw ZException("sparse linear assignment cost matrix is infeasible");
  }

  std::vector<int32_t> colForRow(csr.n, -1);
  std::vector<int32_t> rowForCol(csr.n, -1);
  std::vector<int32_t> freeRows(csr.n, -1);
  std::vector<double> colReduction(csr.n, 0.0);

  size_t numFreeRows = sparseColumnReductionAndTransfer(csr, freeRows, colForRow, rowForCol, colReduction);
  for (int pass = 0; numFreeRows > 0 && pass < 2; ++pass) {
    numFreeRows = sparseAugmentingRowReduction(csr, numFreeRows, freeRows, colForRow, rowForCol, colReduction);
  }
  if (numFreeRows > 0) {
    sparseAugment(csr, numFreeRows, freeRows, colForRow, rowForCol, colReduction);
  }

  ZLinearAssignmentResult result;
  result.rowToCol = std::move(colForRow);
  result.colToRow = std::move(rowForCol);
  result.cost = sparseAssignmentCost(csr, result.rowToCol);
  return result;
}

} // namespace nim
