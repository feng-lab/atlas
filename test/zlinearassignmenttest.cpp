#include "zlinearassignment.h"
#include "zexception.h"
#include "ztest.h"

#include <QFile>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr size_t kCostEpsRows = 368;
constexpr size_t kCostEpsCols = 368;
constexpr double kCostEpsExpectedCost = 224.8899507294651;
constexpr size_t kSyntheticScanRegressionSize = 1024;
constexpr double kSyntheticScanRegressionExpectedCost = 1684.2179999999998;

struct SquareAssignmentCase
{
  std::vector<double> costs;
  size_t n = 0;
  double expectedCost = 0.0;
  std::vector<int32_t> rowToCol;
  std::vector<int32_t> colToRow;
};

struct CsrFixture
{
  std::vector<int32_t> indptr;
  std::vector<int32_t> indices;
  std::vector<double> data;
};

struct CooEntry
{
  int32_t row = 0;
  int32_t col = 0;
  double cost = 0.0;
};

[[nodiscard]] std::vector<SquareAssignmentCase> lapReferenceSquareCases()
{
  return {
    {{1000, 2,    11, 10, 8,  7,    6,  5,  6, 1000, 1,    8,  8,  4,  6,    7,  5,  12, 1000, 11,  8, 12,
      3,    11,   11, 9,  10, 1000, 1,  9,  8, 10,   11,   11, 9,  4,  1000, 2,  10, 9,  12,   8,   5, 2,
      11,   1000, 11, 9,  10, 11,   12, 10, 9, 12,   1000, 3,  10, 10, 10,   10, 6,  3,  1,    1000},
     8,                                                                                                       17.0,
     {1, 2, 0, 4, 5, 3, 7, 6},
     {2, 0, 1, 5, 3, 4, 7, 6}                                                                                                                   },
    {{1000, 4, 1, 1, 1000, 3, 5, 1, 1000},                                                                 3, 3.0,    {2, 0, 1},    {1, 2, 0}   },
    {{5, 1000, 3, 1000, 2, 2, 1, 5, 1000},                                                                 3, 6.0,    {2, 1, 0},    {2, 1, 0}   },
    {{1000, 1001, 1000, 1000, 1000, 1001, 1, 2, 3},                                                        3, 2001.0, {2, 1, 0},    {2, 1, 0}   },
    {{10, 10, 13, 4, 8, 8, 8, 5, 8},                                                                       3, 22.0,   {2, 0, 1},    {1, 2, 0}   },
    {{11, 10, 6, 10, 11, 11, 11, 12, 15},                                                                  3, 28.0,   {2, 0, 1},    {1, 2, 0}   },
    {{12, 4, 9, 16, 15, 14, 19, 13, 17},                                                                   3, 37.0,   {1, 0, 2},    {1, 0, 2}   },
    {{2, 5, 7, 7, 10, 12, 1, 5, 9},                                                                        3, 18.0,   {2, 1, 0},    {2, 1, 0}   },
    {{10, 6, 14, 1, 17, 18, 17, 15, 14, 17, 15, 8, 11, 13, 11, 4},                                         4, 41.0,   {1, 2, 0, 3}, {2, 0, 1, 3}},
    {{10, 10, 13, 4, 8, 8, 8, 5, 8},                                                                       3, 22.0,   {2, 0, 1},    {1, 2, 0}   },
    {{2, 5, 7, 7, 10, 12, 1, 5, 9},                                                                        3, 18.0,   {2, 1, 0},    {2, 1, 0}   },
  };
}

[[nodiscard]] std::vector<unsigned char> readFixtureBytes(const QString& relativePath)
{
  const QString path = nim::getTestDataDir().filePath(relativePath);
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    throw nim::ZException("failed to open test fixture: " + path.toStdString());
  }
  const auto bytes = file.readAll();
  CHECK(!bytes.isEmpty());

  const auto* first = reinterpret_cast<const unsigned char*>(bytes.constData());
  return {first, first + bytes.size()};
}

[[nodiscard]] std::string inflateGzip(std::span<const unsigned char> compressed)
{
  z_stream stream{};
  stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());

  int err = inflateInit2(&stream, MAX_WBITS + 16);
  CHECK_EQ(err, Z_OK);

  std::string output;
  std::array<char, 32768> buffer{};
  do {
    stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
    stream.avail_out = static_cast<uInt>(buffer.size());
    err = inflate(&stream, Z_NO_FLUSH);
    CHECK(err == Z_OK || err == Z_STREAM_END);
    output.append(buffer.data(), buffer.size() - stream.avail_out);
  } while (err != Z_STREAM_END);

  err = inflateEnd(&stream);
  CHECK_EQ(err, Z_OK);
  return output;
}

[[nodiscard]] std::vector<double> parseCsvDoubles(std::string_view csv)
{
  std::vector<double> values;
  values.reserve(kCostEpsRows * kCostEpsCols);

  const char* cursor = csv.data();
  const char* const end = csv.data() + csv.size();
  while (cursor < end) {
    while (cursor < end && (*cursor == ',' || *cursor == '\n' || *cursor == '\r' || std::isspace(*cursor))) {
      ++cursor;
    }
    if (cursor == end) {
      break;
    }

    char* next = nullptr;
    values.push_back(std::strtod(cursor, &next));
    CHECK_NE(next, cursor);
    cursor = next;
  }

  return values;
}

[[nodiscard]] std::vector<double> loadCostEpsMatrix()
{
  const std::vector<unsigned char> compressed = readFixtureBytes(QStringLiteral("linear_assignment/cost_eps.csv.gz"));
  std::vector<double> costs = parseCsvDoubles(inflateGzip(compressed));
  CHECK_EQ(costs.size(), kCostEpsRows * kCostEpsCols);
  return costs;
}

[[nodiscard]] std::vector<double> makeSyntheticBenchmarkCost(size_t n)
{
  std::vector<double> costs(n * n);
  for (size_t row = 0; row < n; ++row) {
    for (size_t col = 0; col < n; ++col) {
      const size_t mixed = (row * 1315423911u) ^ (col * 2654435761u) ^ ((row + col) * 2246822519u);
      costs[row * n + col] = static_cast<double>(mixed % 1000003u) / 1000.0;
    }
  }
  return costs;
}

[[nodiscard]] double costAt(const std::vector<double>& costs, size_t rows, size_t cols, size_t row, size_t col)
{
  (void)rows;
  return costs[row * cols + col];
}

[[nodiscard]] double
assignedCost(const std::vector<double>& costs, size_t rows, size_t cols, const std::vector<int32_t>& rowToCol)
{
  double total = 0.0;
  for (size_t row = 0; row < rows; ++row) {
    const int32_t col = rowToCol[row];
    if (col >= 0) {
      total += costAt(costs, rows, cols, row, static_cast<size_t>(col));
    }
  }
  return total;
}

void expectValidAssignment(const nim::ZLinearAssignmentResult& result, size_t rows, size_t cols)
{
  ASSERT_EQ(result.rowToCol.size(), rows);
  ASSERT_EQ(result.colToRow.size(), cols);
  size_t matchedRows = 0;
  size_t matchedCols = 0;
  std::vector<char> usedCols(cols, char{0});
  for (size_t row = 0; row < rows; ++row) {
    const int32_t col = result.rowToCol[row];
    if (col < 0) {
      continue;
    }
    ASSERT_LT(static_cast<size_t>(col), cols);
    EXPECT_FALSE(usedCols[static_cast<size_t>(col)]);
    usedCols[static_cast<size_t>(col)] = char{1};
    ASSERT_EQ(result.colToRow[static_cast<size_t>(col)], static_cast<int32_t>(row));
    ++matchedRows;
  }
  for (size_t col = 0; col < cols; ++col) {
    const int32_t row = result.colToRow[col];
    if (row < 0) {
      continue;
    }
    ASSERT_LT(static_cast<size_t>(row), rows);
    ASSERT_EQ(result.rowToCol[static_cast<size_t>(row)], static_cast<int32_t>(col));
    ++matchedCols;
  }
  EXPECT_EQ(matchedRows, matchedCols);
  EXPECT_EQ(matchedRows, std::min(rows, cols));
}

void expectAssignment(const nim::ZLinearAssignmentResult& result,
                      const std::vector<double>& costs,
                      size_t rows,
                      size_t cols,
                      double expectedCost,
                      std::span<const int32_t> expectedRowToCol,
                      std::span<const int32_t> expectedColToRow)
{
  expectValidAssignment(result, rows, cols);
  EXPECT_NEAR(result.cost, expectedCost, 1e-10);
  EXPECT_NEAR(result.cost, assignedCost(costs, rows, cols, result.rowToCol), 1e-10);
  if (!expectedRowToCol.empty()) {
    EXPECT_EQ(result.rowToCol, (std::vector<int32_t>{expectedRowToCol.begin(), expectedRowToCol.end()}));
  }
  if (!expectedColToRow.empty()) {
    EXPECT_EQ(result.colToRow, (std::vector<int32_t>{expectedColToRow.begin(), expectedColToRow.end()}));
  }
}

[[nodiscard]] CsrFixture makeFullCsr(size_t rows, size_t cols, const std::vector<double>& costs)
{
  CHECK_EQ(costs.size(), rows * cols);
  CHECK_LE(cols, static_cast<size_t>(std::numeric_limits<int32_t>::max()));
  CHECK_LE(rows * cols, static_cast<size_t>(std::numeric_limits<int32_t>::max()));

  CsrFixture csr;
  csr.indptr.assign(rows + 1, 0);
  csr.indices.reserve(rows * cols);
  csr.data.reserve(rows * cols);
  for (size_t row = 0; row < rows; ++row) {
    csr.indptr[row] = static_cast<int32_t>(csr.indices.size());
    for (size_t col = 0; col < cols; ++col) {
      csr.indices.push_back(static_cast<int32_t>(col));
      csr.data.push_back(costAt(costs, rows, cols, row, col));
    }
  }
  csr.indptr[rows] = static_cast<int32_t>(csr.indices.size());
  return csr;
}

[[nodiscard]] CsrFixture makeFiniteCsr(size_t rows, size_t cols, const std::vector<double>& costs)
{
  CHECK_EQ(costs.size(), rows * cols);
  CHECK_LE(cols, static_cast<size_t>(std::numeric_limits<int32_t>::max()));

  CsrFixture csr;
  csr.indptr.assign(rows + 1, 0);
  for (size_t row = 0; row < rows; ++row) {
    csr.indptr[row] = static_cast<int32_t>(csr.indices.size());
    for (size_t col = 0; col < cols; ++col) {
      const double cost = costAt(costs, rows, cols, row, col);
      if (std::isfinite(cost)) {
        csr.indices.push_back(static_cast<int32_t>(col));
        csr.data.push_back(cost);
      }
    }
  }
  csr.indptr[rows] = static_cast<int32_t>(csr.indices.size());
  return csr;
}

[[nodiscard]] CsrFixture makeCsrFromSortedEntries(size_t rows, std::vector<CooEntry> entries)
{
  std::sort(entries.begin(), entries.end(), [](const CooEntry& lhs, const CooEntry& rhs) {
    if (lhs.row != rhs.row) {
      return lhs.row < rhs.row;
    }
    return lhs.col < rhs.col;
  });

  CsrFixture csr;
  csr.indptr.assign(rows + 1, 0);
  size_t entryIndex = 0;
  for (size_t row = 0; row < rows; ++row) {
    csr.indptr[row] = static_cast<int32_t>(csr.indices.size());
    while (entryIndex < entries.size() && entries[entryIndex].row == static_cast<int32_t>(row)) {
      csr.indices.push_back(entries[entryIndex].col);
      csr.data.push_back(entries[entryIndex].cost);
      ++entryIndex;
    }
  }
  CHECK_EQ(entryIndex, entries.size());
  csr.indptr[rows] = static_cast<int32_t>(csr.indices.size());
  return csr;
}

[[nodiscard]] double bruteForceMinCostRowsLeCols(const std::vector<double>& costs, size_t rows, size_t cols)
{
  std::vector<int32_t> columns(cols);
  std::iota(columns.begin(), columns.end(), int32_t{0});
  double best = kInf;
  do {
    double total = 0.0;
    bool feasible = true;
    for (size_t row = 0; row < rows; ++row) {
      const double c = costAt(costs, rows, cols, row, static_cast<size_t>(columns[row]));
      if (!std::isfinite(c)) {
        feasible = false;
        break;
      }
      total += c;
    }
    if (feasible) {
      best = std::min(best, total);
    }
  } while (std::next_permutation(columns.begin(), columns.end()));
  return best;
}

[[nodiscard]] double bruteForceMinCost(const std::vector<double>& costs, size_t rows, size_t cols)
{
  if (rows <= cols) {
    return bruteForceMinCostRowsLeCols(costs, rows, cols);
  }

  std::vector<double> transposed(rows * cols);
  for (size_t row = 0; row < rows; ++row) {
    for (size_t col = 0; col < cols; ++col) {
      transposed[col * rows + row] = costAt(costs, rows, cols, row, col);
    }
  }
  return bruteForceMinCostRowsLeCols(transposed, cols, rows);
}

} // namespace

TEST(ZLinearAssignment, DenseSquareFastMatchesLapReferenceCases)
{
  using namespace nim;

  for (const SquareAssignmentCase& testCase : lapReferenceSquareCases()) {
    const ZLinearAssignmentResult result = solveLinearAssignment(testCase.costs, testCase.n, testCase.n);
    expectAssignment(result,
                     testCase.costs,
                     testCase.n,
                     testCase.n,
                     testCase.expectedCost,
                     testCase.rowToCol,
                     testCase.colToRow);
  }
}

TEST(ZLinearAssignment, SparseCsrFullMatrixMatchesLapReferenceCases)
{
  using namespace nim;

  for (const SquareAssignmentCase& testCase : lapReferenceSquareCases()) {
    const CsrFixture csr = makeFullCsr(testCase.n, testCase.n, testCase.costs);
    const ZLinearAssignmentResult result =
      solveLinearAssignmentCsr(testCase.n, testCase.n, csr.indptr, csr.indices, csr.data);
    expectAssignment(result,
                     testCase.costs,
                     testCase.n,
                     testCase.n,
                     testCase.expectedCost,
                     testCase.rowToCol,
                     testCase.colToRow);
  }
}

TEST(ZLinearAssignment, DenseSquareFastMatchesLapEpsRegression)
{
  using namespace nim;

  const std::vector<double> costs = loadCostEpsMatrix();
  const ZLinearAssignmentResult result = solveLinearAssignment(costs, kCostEpsRows, kCostEpsCols);
  expectValidAssignment(result, kCostEpsRows, kCostEpsCols);

  // This matrix catches a square LAPJV scan-cursor regression that produced a
  // complete but non-optimal assignment.
  EXPECT_NEAR(result.cost, kCostEpsExpectedCost, 1e-10);
  EXPECT_NEAR(result.cost, assignedCost(costs, kCostEpsRows, kCostEpsCols, result.rowToCol), 1e-10);
}

TEST(ZLinearAssignment, DenseSquareFastHandlesSyntheticScanRegression)
{
  using namespace nim;

  const std::vector<double> costs = makeSyntheticBenchmarkCost(kSyntheticScanRegressionSize);
  const ZLinearAssignmentResult result =
    solveLinearAssignment(costs, kSyntheticScanRegressionSize, kSyntheticScanRegressionSize);
  expectValidAssignment(result, kSyntheticScanRegressionSize, kSyntheticScanRegressionSize);

  // This benchmark-shaped matrix caught a scan-list cursor regression that
  // could create a predecessor cycle during final augmentation.
  EXPECT_NEAR(result.cost, kSyntheticScanRegressionExpectedCost, 1e-10);
  EXPECT_NEAR(result.cost,
              assignedCost(costs, kSyntheticScanRegressionSize, kSyntheticScanRegressionSize, result.rowToCol),
              1e-10);
}

TEST(ZLinearAssignment, DenseRectangularMatchesBruteForce)
{
  using namespace nim;

  const std::vector<double> costs = {
    8,
    2,
    5,
    7,
    3,
    6,
    4,
    9,
    1,
    8,
    5,
    7,
    3,
    6,
    2,
  };

  const ZLinearAssignmentResult result = solveLinearAssignment(costs, 3, 5);
  expectValidAssignment(result, 3, 5);
  EXPECT_DOUBLE_EQ(result.cost, bruteForceMinCost(costs, 3, 5));
}

TEST(ZLinearAssignment, DenseTallRectangularLeavesExtraRowsUnmatched)
{
  using namespace nim;

  const std::vector<double> costs = {
    4,
    7,
    2,
    9,
    6,
    1,
    3,
    5,
  };

  const ZLinearAssignmentResult result = solveLinearAssignment(costs, 4, 2);
  expectValidAssignment(result, 4, 2);
  EXPECT_DOUBLE_EQ(result.cost, bruteForceMinCost(costs, 4, 2));
  EXPECT_EQ(result.matchedRows().size(), 2);
}

TEST(ZLinearAssignment, DenseRectangularMatchesLapArrLoopRegression)
{
  using namespace nim;

  const std::vector<double> values = {0.2593883482138951146,
                                      0.3080381437461217620,
                                      0.1976243020727339317,
                                      0.2462740976049606068,
                                      0.4203993396282833528,
                                      0.4286184525458427985,
                                      0.1706431415909629434,
                                      0.2192929371231896185,
                                      0.2117769622802734286,
                                      0.2604267578125001315};
  const std::vector<int32_t> rows = {0, 0, 1, 1, 2, 2, 5, 5, 6, 6};
  const std::vector<int32_t> cols = {0, 1, 0, 1, 1, 2, 0, 1, 0, 1};

  std::vector<double> costs(7 * 3, 1000.0);
  for (size_t i = 0; i < values.size(); ++i) {
    costs[static_cast<size_t>(rows[i]) * 3 + static_cast<size_t>(cols[i])] = values[i];
  }

  const ZLinearAssignmentResult result = solveLinearAssignment(costs, 7, 3);
  expectValidAssignment(result, 7, 3);
  EXPECT_NEAR(result.cost, 0.8455356917416, 1e-10);
  EXPECT_NEAR(result.cost, assignedCost(costs, 7, 3, result.rowToCol), 1e-10);
  EXPECT_TRUE(result.colToRow == (std::vector<int32_t>{5, 1, 2}) || result.colToRow == (std::vector<int32_t>{1, 5, 2}));
}

TEST(ZLinearAssignment, MaximizeMatchesBruteForceAfterNegation)
{
  using namespace nim;

  const std::vector<double> profits = {
    5,
    1,
    9,
    4,
    8,
    2,
    7,
    6,
    3,
  };
  std::vector<double> negated = profits;
  for (double& value : negated) {
    value = -value;
  }

  ZLinearAssignmentOptions options;
  options.objective = ZLinearAssignmentObjective::Maximize;
  const ZLinearAssignmentResult result = solveLinearAssignment(profits, 3, 3, options);
  expectValidAssignment(result, 3, 3);
  EXPECT_DOUBLE_EQ(result.cost, -bruteForceMinCost(negated, 3, 3));
}

TEST(ZLinearAssignment, CostLimitAllowsUnmatchedRowsAndColumns)
{
  using namespace nim;

  const std::vector<double> costs = {
    1000,
    2,
    11,
    6,
    1000,
    1,
    5,
    12,
    1000,
  };
  ZLinearAssignmentOptions options;
  options.costLimit = 4.99;

  const ZLinearAssignmentResult result = solveLinearAssignment(costs, 3, 3, options);
  EXPECT_EQ(result.rowToCol, (std::vector<int32_t>{1, 2, -1}));
  EXPECT_EQ(result.colToRow, (std::vector<int32_t>{-1, 0, 1}));
  EXPECT_DOUBLE_EQ(result.cost, 3.0);
}

TEST(ZLinearAssignment, CostLimitIncludesEqualCostAssignments)
{
  using namespace nim;

  ZLinearAssignmentOptions options;
  options.costLimit = 1.0;

  const ZLinearAssignmentResult result = solveLinearAssignment(std::vector<double>{1.0}, 1, 1, options);
  EXPECT_EQ(result.rowToCol, (std::vector<int32_t>{0}));
  EXPECT_EQ(result.colToRow, (std::vector<int32_t>{0}));
  EXPECT_DOUBLE_EQ(result.cost, 1.0);
}

TEST(ZLinearAssignment, CostLimitRejectsImmediatelyLargerCost)
{
  using namespace nim;

  constexpr double costLimit = 1.0;
  ZLinearAssignmentOptions options;
  options.costLimit = costLimit;
  const double overLimit = std::nextafter(costLimit, kInf);

  const ZLinearAssignmentResult result = solveLinearAssignment(std::vector<double>{overLimit}, 1, 1, options);
  EXPECT_EQ(result.rowToCol, (std::vector<int32_t>{-1}));
  EXPECT_EQ(result.colToRow, (std::vector<int32_t>{-1}));
  EXPECT_DOUBLE_EQ(result.cost, 0.0);
}

TEST(ZLinearAssignment, CostLimitOverLimitEdgeCannotDisplaceEligibleMatches)
{
  using namespace nim;

  constexpr double costLimit = 1.0;
  ZLinearAssignmentOptions options;
  options.costLimit = costLimit;
  const double overLimit = std::nextafter(costLimit, kInf);
  const std::vector<double> costs = {
    1.0,
    overLimit,
    1.0,
    1.0,
    1.0,
    1.0,
  };

  const ZLinearAssignmentResult result = solveLinearAssignment(costs, 2, 3, options);
  expectValidAssignment(result, 2, 3);
  for (size_t row = 0; row < result.rowToCol.size(); ++row) {
    ASSERT_GE(result.rowToCol[row], 0);
    EXPECT_LE(costAt(costs, 2, 3, row, static_cast<size_t>(result.rowToCol[row])), costLimit);
  }
  EXPECT_DOUBLE_EQ(result.cost, 2.0);
}

TEST(ZLinearAssignment, CostLimitInclusiveTiePreservesAvailableMatch)
{
  using namespace nim;

  constexpr double costLimit = 1.0;
  ZLinearAssignmentOptions options;
  options.costLimit = costLimit;
  const double belowLimit = std::nextafter(costLimit, -kInf);
  const double overLimit = std::nextafter(costLimit, kInf);
  const std::vector<double> costs = {
    belowLimit,
    belowLimit,
    belowLimit,
    costLimit,
    overLimit,
    overLimit,
  };

  const ZLinearAssignmentResult result = solveLinearAssignment(costs, 2, 3, options);
  expectValidAssignment(result, 2, 3);
  EXPECT_TRUE(result.rowToCol[0] == 1 || result.rowToCol[0] == 2);
  EXPECT_EQ(result.rowToCol[1], 0);
  EXPECT_EQ(result.colToRow[0], 1);
  EXPECT_DOUBLE_EQ(result.cost, belowLimit + costLimit);
}

TEST(ZLinearAssignment, DenseInfCostMatrixUsesRectangularPathAndRejectsInfeasible)
{
  using namespace nim;

  const std::vector<double> costs = {
    11, 20, kInf, kInf, kInf, 12, kInf, 12, kInf, kInf, kInf, 11, 10,
    15, 9,  15,   kInf, kInf, 22, kInf, 13, kInf, kInf, kInf, 15,
  };
  const ZLinearAssignmentResult result = solveLinearAssignment(costs, 5, 5);
  expectValidAssignment(result, 5, 5);
  EXPECT_EQ(result.rowToCol, (std::vector<int32_t>{0, 2, 1, 3, 4}));
  EXPECT_DOUBLE_EQ(result.cost, 71.0);

  const std::vector<double> infeasible = {
    0, 0, 0, kInf, kInf, kInf, kInf, kInf, 0, 0, kInf, kInf, kInf, 0, 0, kInf, kInf, kInf, 0, 0, 0, 0, 0, kInf, kInf,
  };
  EXPECT_THROW((void)solveLinearAssignment(infeasible, 5, 5), ZException);
}

TEST(ZLinearAssignment, DenseInfeasibleInfCasesRaise)
{
  using namespace nim;

  const std::vector<std::vector<double>> infeasibleCases = {
    {0,  0,    0,  kInf, kInf, kInf, kInf, kInf, 0, 0,  kInf, kInf, kInf, 0,    0,    kInf, kInf, kInf, 0,  0,  0,  0,    0,  kInf, kInf},
    {19, 22,   16, kInf, kInf, kInf, kInf, kInf, 4, 13, kInf, kInf, kInf,
     3,                                                                         14,   kInf, kInf, kInf, 10, 12, 11, 14,   13, kInf, kInf},
    {0,  kInf, 0,  0,    kInf, kInf, kInf, 0,    0, 0,  kInf, kInf, kInf,
     0,                                                                         kInf, kInf, kInf, kInf, 0,  0,  0,  kInf, 0,  kInf, kInf},
    {0,  0,    0,  0,    kInf, kInf, kInf, 0,    0, 0,  kInf, kInf, kInf, kInf, kInf, kInf, kInf, kInf, 0,  0,  0,  0,    0,  kInf, kInf},
    std::vector<double>(25, kInf),
  };

  for (const std::vector<double>& costs : infeasibleCases) {
    EXPECT_THROW((void)solveLinearAssignment(costs, 5, 5), ZException);
  }
}

TEST(ZLinearAssignment, DenseValidationRejectsInvalidNumbers)
{
  using namespace nim;

  EXPECT_THROW(
    (void)solveLinearAssignment(std::vector<double>{0, std::numeric_limits<double>::quiet_NaN(), 1, 2}, 2, 2),
    ZException);
  EXPECT_THROW((void)solveLinearAssignment(std::vector<double>{0, -kInf, 1, 2}, 2, 2), ZException);

  ZLinearAssignmentOptions maximize;
  maximize.objective = ZLinearAssignmentObjective::Maximize;
  EXPECT_THROW((void)solveLinearAssignment(std::vector<double>{0, kInf, 1, 2}, 2, 2, maximize), ZException);
}

TEST(ZLinearAssignment, DenseOverloadsConvertArithmeticCostTypes)
{
  using namespace nim;

  const std::vector<float> floatCosts = {
    4.0f,
    1.0f,
    2.0f,
    3.0f,
  };
  const ZLinearAssignmentResult floatResult = solveLinearAssignment(floatCosts, 2, 2);
  expectValidAssignment(floatResult, 2, 2);
  EXPECT_EQ(floatResult.rowToCol, (std::vector<int32_t>{1, 0}));
  EXPECT_DOUBLE_EQ(floatResult.cost, 3.0);

  const std::vector<int16_t> intCosts = {
    7,
    5,
    4,
    9,
  };
  const ZLinearAssignmentResult intResult =
    solveLinearAssignment(std::span<const int16_t>(intCosts.data(), intCosts.size()), 2, 2);
  expectValidAssignment(intResult, 2, 2);
  EXPECT_EQ(intResult.rowToCol, (std::vector<int32_t>{1, 0}));
  EXPECT_DOUBLE_EQ(intResult.cost, 9.0);
}

TEST(ZLinearAssignment, SparseCsrMatchesDenseReference)
{
  using namespace nim;

  const std::vector<int32_t> indptr = {0, 2, 4, 8, 10, 12};
  const std::vector<int32_t> indices = {0, 1, 0, 2, 1, 2, 3, 4, 0, 3, 0, 4};
  const std::vector<double> data = {11, 20, 12, 12, 11, 10, 15, 9, 15, 22, 13, 15};

  ZLinearAssignmentCsrView view;
  view.rows = 5;
  view.cols = 5;
  view.indptr = indptr;
  view.indices = indices;
  view.costs = data;

  const ZLinearAssignmentResult result = solveLinearAssignmentCsr(view);
  expectValidAssignment(result, 5, 5);
  EXPECT_EQ(result.rowToCol, (std::vector<int32_t>{0, 2, 1, 3, 4}));
  EXPECT_DOUBLE_EQ(result.cost, 71.0);
}

TEST(ZLinearAssignment, SparseCsrWideRectangularMatchesDense)
{
  using namespace nim;

  const std::vector<double> costs = {
    8,
    2,
    kInf,
    7,
    3,
    6,
    kInf,
    1,
    8,
    5,
    kInf,
    6,
    2,
    4,
    9,
  };
  const CsrFixture csr = makeFiniteCsr(3, 5, costs);

  const ZLinearAssignmentResult sparse = solveLinearAssignmentCsr(3, 5, csr.indptr, csr.indices, csr.data);
  const ZLinearAssignmentResult dense = solveLinearAssignment(costs, 3, 5);
  expectValidAssignment(sparse, 3, 5);
  expectValidAssignment(dense, 3, 5);
  EXPECT_EQ(sparse.rowToCol, dense.rowToCol);
  EXPECT_EQ(sparse.colToRow, dense.colToRow);
  EXPECT_DOUBLE_EQ(sparse.cost, dense.cost);
  EXPECT_DOUBLE_EQ(sparse.cost, bruteForceMinCost(costs, 3, 5));
}

TEST(ZLinearAssignment, SparseCsrTallRectangularMatchesDense)
{
  using namespace nim;

  const std::vector<double> costs = {
    4,
    7,
    2,
    kInf,
    6,
    1,
    3,
    5,
  };
  const CsrFixture csr = makeFiniteCsr(4, 2, costs);

  const ZLinearAssignmentResult sparse = solveLinearAssignmentCsr(4, 2, csr.indptr, csr.indices, csr.data);
  const ZLinearAssignmentResult dense = solveLinearAssignment(costs, 4, 2);
  expectValidAssignment(sparse, 4, 2);
  expectValidAssignment(dense, 4, 2);
  EXPECT_EQ(sparse.rowToCol, dense.rowToCol);
  EXPECT_EQ(sparse.colToRow, dense.colToRow);
  EXPECT_DOUBLE_EQ(sparse.cost, dense.cost);
  EXPECT_DOUBLE_EQ(sparse.cost, bruteForceMinCost(costs, 4, 2));
  EXPECT_EQ(sparse.matchedRows().size(), 2);
}

TEST(ZLinearAssignment, SparseCsrMaskedCasesMatchDenseAndBruteForce)
{
  using namespace nim;

  auto runCase = [](std::vector<double> costs, size_t n) {
    std::vector<int32_t> indptr(n + 1, 0);
    std::vector<int32_t> indices;
    std::vector<double> data;
    for (size_t row = 0; row < n; ++row) {
      indptr[row] = static_cast<int32_t>(indices.size());
      for (size_t col = 0; col < n; ++col) {
        const double cost = costs[row * n + col];
        if (std::isfinite(cost)) {
          indices.push_back(static_cast<int32_t>(col));
          data.push_back(cost);
        }
      }
    }
    indptr[n] = static_cast<int32_t>(indices.size());

    const ZLinearAssignmentResult sparse = solveLinearAssignmentCsr(n, n, indptr, indices, data);
    const ZLinearAssignmentResult dense = solveLinearAssignment(costs, n, n);
    expectValidAssignment(sparse, n, n);
    expectValidAssignment(dense, n, n);
    EXPECT_NEAR(sparse.cost, dense.cost, 1e-10);
    EXPECT_NEAR(sparse.cost, bruteForceMinCost(costs, n, n), 1e-10);
  };

  runCase({11, 20, kInf, kInf, kInf, 12, kInf, 12, kInf, kInf, kInf, 11, 10,
           15, 9,  15,   kInf, kInf, 22, kInf, 13, kInf, kInf, kInf, 15},
          5);

  std::vector<double> cyclic(8 * 8, kInf);
  for (size_t row = 0; row < 8; ++row) {
    cyclic[row * 8 + row] = 100.0 + static_cast<double>(row);
    cyclic[row * 8 + ((row + 1) % 8)] = 1.0 + static_cast<double>(row);
  }
  runCase(std::move(cyclic), 8);

  std::vector<double> nearDense = {kInf, 1, 7, 7, 7, 1,    kInf, 1, 7, 7, 7, 1,   kInf,
                                   1,    7, 7, 7, 1, kInf, 1,    1, 7, 7, 7, kInf};
  runCase(std::move(nearDense), 5);
}

TEST(ZLinearAssignment, SparseCsrMatchesLapArrLoopRegression)
{
  using namespace nim;

  const std::vector<double> values = {0.2593883482138951146,
                                      0.3080381437461217620,
                                      0.1976243020727339317,
                                      0.2462740976049606068,
                                      0.4203993396282833528,
                                      0.4286184525458427985,
                                      0.1706431415909629434,
                                      0.2192929371231896185,
                                      0.2117769622802734286,
                                      0.2604267578125001315};
  const std::vector<int32_t> originalRows = {0, 0, 1, 1, 2, 2, 5, 5, 6, 6};
  const std::vector<int32_t> originalCols = {0, 1, 0, 1, 1, 2, 0, 1, 0, 1};

  std::vector<CooEntry> entries;
  std::vector<double> extendedCosts(10 * 10, kInf);
  auto addEntry = [&](int32_t row, int32_t col, double cost) {
    entries.push_back(CooEntry{row, col, cost});
    extendedCosts[static_cast<size_t>(row) * 10 + static_cast<size_t>(col)] = cost;
  };

  for (size_t i = 0; i < values.size(); ++i) {
    addEntry(originalRows[i], originalCols[i], values[i]);
  }
  for (int32_t row = 0; row < 7; ++row) {
    addEntry(row, 3 + row, 1000.0);
  }
  for (int32_t col = 0; col < 3; ++col) {
    addEntry(7 + col, col, 1000.0);
  }
  for (size_t i = 0; i < values.size(); ++i) {
    addEntry(7 + originalCols[i], 3 + originalRows[i], 0.0);
  }

  const CsrFixture csr = makeCsrFromSortedEntries(10, std::move(entries));
  const ZLinearAssignmentResult result = solveLinearAssignmentCsr(10, 10, csr.indptr, csr.indices, csr.data);
  expectValidAssignment(result, 10, 10);
  EXPECT_NEAR(result.cost, 4000.8455356917416, 1e-10);
  EXPECT_NEAR(result.cost, assignedCost(extendedCosts, 10, 10, result.rowToCol), 1e-10);

  const std::vector<int32_t> croppedColToRow(result.colToRow.begin(), result.colToRow.begin() + 3);
  EXPECT_TRUE(croppedColToRow == (std::vector<int32_t>{5, 1, 2}) || croppedColToRow == (std::vector<int32_t>{1, 5, 2}));
}

TEST(ZLinearAssignment, SparseCsrDenseLikeFixtureMatchesDenseReference)
{
  using namespace nim;

  const std::vector<double> costs = loadCostEpsMatrix();
  std::vector<int32_t> indptr(kCostEpsRows + 1, 0);
  std::vector<int32_t> indices(kCostEpsRows * kCostEpsCols, 0);
  for (size_t row = 0; row < kCostEpsRows; ++row) {
    indptr[row] = static_cast<int32_t>(row * kCostEpsCols);
    for (size_t col = 0; col < kCostEpsCols; ++col) {
      indices[row * kCostEpsCols + col] = static_cast<int32_t>(col);
    }
  }
  indptr[kCostEpsRows] = static_cast<int32_t>(indices.size());

  ZLinearAssignmentCsrView view;
  view.rows = kCostEpsRows;
  view.cols = kCostEpsCols;
  view.indptr = indptr;
  view.indices = indices;
  view.costs = costs;

  const ZLinearAssignmentResult result = solveLinearAssignmentCsr(view);
  expectValidAssignment(result, kCostEpsRows, kCostEpsCols);
  EXPECT_NEAR(result.cost, kCostEpsExpectedCost, 1e-10);
}

TEST(ZLinearAssignment, SparseCsrOverloadsConvertIndexAndCostTypes)
{
  using namespace nim;

  const std::vector<int64_t> indptr = {0, 2, 4, 8, 10, 12};
  const std::vector<uint16_t> indices = {0, 1, 0, 2, 1, 2, 3, 4, 0, 3, 0, 4};
  const std::vector<float> data = {11, 20, 12, 12, 11, 10, 15, 9, 15, 22, 13, 15};

  const ZLinearAssignmentResult result = solveLinearAssignmentCsr(5, 5, indptr, indices, data);
  expectValidAssignment(result, 5, 5);
  EXPECT_EQ(result.rowToCol, (std::vector<int32_t>{0, 2, 1, 3, 4}));
  EXPECT_DOUBLE_EQ(result.cost, 71.0);

  const std::vector<int64_t> badIndices = {0, static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1};
  EXPECT_THROW(
    (void)solveLinearAssignmentCsr(2, 2, std::vector<int64_t>{0, 2, 2}, badIndices, std::vector<double>{1, 2}),
    ZException);
}

TEST(ZLinearAssignment, SparseCsrRejectsMalformedInput)
{
  using namespace nim;

  ZLinearAssignmentCsrView nonSquare;
  nonSquare.rows = 2;
  nonSquare.cols = 3;
  nonSquare.indptr = std::span<const int32_t>();
  nonSquare.indices = std::span<const int32_t>();
  nonSquare.costs = std::span<const double>();
  EXPECT_THROW((void)solveLinearAssignmentCsr(nonSquare), ZException);

  const std::vector<int32_t> indptr = {0, 2, 2};
  const std::vector<int32_t> duplicateIndices = {0, 0};
  const std::vector<double> data = {1, 2};
  ZLinearAssignmentCsrView duplicate;
  duplicate.rows = 2;
  duplicate.cols = 2;
  duplicate.indptr = indptr;
  duplicate.indices = duplicateIndices;
  duplicate.costs = data;
  EXPECT_THROW((void)solveLinearAssignmentCsr(duplicate), ZException);

  const CsrFixture infeasible = makeFiniteCsr(5, 5, {0, 0, 0,    kInf, kInf, kInf, kInf, kInf, 0, 0, kInf, kInf, kInf,
                                                     0, 0, kInf, kInf, kInf, 0,    0,    0,    0, 0, kInf, kInf});
  EXPECT_THROW((void)solveLinearAssignmentCsr(5, 5, infeasible.indptr, infeasible.indices, infeasible.data),
               ZException);

  const std::vector<int32_t> finiteIndptr = {0, 2, 4};
  const std::vector<int32_t> finiteIndices = {0, 1, 0, 1};
  const std::vector<double> nonFiniteData = {1, kInf, 2, 3};
  EXPECT_THROW((void)solveLinearAssignmentCsr(2, 2, finiteIndptr, finiteIndices, nonFiniteData), ZException);
}
