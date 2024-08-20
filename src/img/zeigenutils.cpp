#include "zeigenutils.h"

#include "zexception.h"
#include "zstringutils.h"

namespace nim {

using namespace Eigen;

// Read numbers in the string to a row vector
// nDataToRead can be used to control the length of the output vector, if actual number of data is less than
// nDataToRead, the rest will be filled with fillValue. Will read all data to the row vector if nDataToRead is -1 If
// strictDelimiter is used, empty data will be filled with fillValue.
RowVectorXd readRowVector(std::string_view line,
                          double fillValue,
                          std::string_view strictDelimiter,
                          std::string_view commentStart,
                          Eigen::Index nDataToRead)
{
  RowVectorXd rowVector;
  // remove comment
  line = absl::StripAsciiWhitespace(removeComment(line, commentStart, true));
  std::vector<std::string_view> parts;
  if (strictDelimiter.empty()) [[likely]] {
    parts = absl::StrSplit(line, absl::ByAnyChar(delimiter_literal), absl::SkipEmpty());
  } else [[unlikely]] {
    parts = absl::StrSplit(line, absl::ByString(strictDelimiter));
  }
  if (parts.empty()) {
    return rowVector;
  }

  if (nDataToRead < 0) {
    nDataToRead = parts.size();
  }
  rowVector.resize(nDataToRead);
  for (Eigen::Index i = 0; i < nDataToRead; ++i) {
    if (static_cast<size_t>(i) >= parts.size()) [[unlikely]] {
      rowVector(i) = fillValue;
    } else [[likely]] {
      auto sv = parts[i];
      if (sv.empty()) [[unlikely]] {
        rowVector(i) = fillValue;
      }

      if (absl::StrContains(sv, "1.#"sv)) [[unlikely]] {
        if (absl::EqualsIgnoreCase(sv, "-1.#inf"sv)) {
          rowVector(i) = -std::numeric_limits<double>::infinity();
        } else if (absl::EqualsIgnoreCase(sv, "1.#inf"sv)) {
          rowVector(i) = std::numeric_limits<double>::infinity();
        } else {
          rowVector(i) = std::numeric_limits<double>::quiet_NaN();
        }
      } else [[likely]] {
        auto res = boost::charconv::from_chars_erange(sv.data(), sv.data() + sv.size(), rowVector(i));
        if (res.ec == std::errc::result_out_of_range) {
          // throw ZException(fmt::format("error: result_out_of_range when converting {} to Real number", sv));
          LOG(WARNING) << fmt::format("warning: result_out_of_range when converting {} to Real number", sv);
        } else if (res.ec == std::errc::invalid_argument) {
          rowVector(i) = std::numeric_limits<double>::quiet_NaN();
        }
      }
    }
  }

  return rowVector;
}

MatrixXd ZEigenUtils::readMatrix(const QString& filename,
                                 double fillValue,
                                 std::string_view strictDelimiter,
                                 std::string_view commentStart)
{
  std::ifstream inputFileStream = openIFStream(filename, std::ios::in);

  auto nRow = std::count(std::istreambuf_iterator<char>(inputFileStream), std::istreambuf_iterator<char>(), '\n') + 1;
  inputFileStream.clear();
  inputFileStream.seekg(0, std::ios::beg);

  if (inputFileStream.bad()) {
    throw ZException(fmt::format("Error while reading file {}", filename), ZException::Option::CheckErrno);
  }

  MatrixXd mat;
  Eigen::Index notEmptyLineIdx = 0;
  std::string line;
  while (std::getline(inputFileStream, line)) {
    if (mat.cols() == 0) { // get number of col and make space in mat
      RowVectorXd rowVector = readRowVector(line, fillValue, strictDelimiter, commentStart, -1);
      if (rowVector.size() == 0) {
        continue;
      }
      mat.resize(nRow, rowVector.size());
      mat.row(notEmptyLineIdx++) = rowVector;
    } else {
      RowVectorXd rowVector = readRowVector(line, fillValue, strictDelimiter, commentStart, mat.cols());
      if (rowVector.size() > 0) {
        mat.row(notEmptyLineIdx++) = rowVector;
      }
    }
  }

  if (inputFileStream.bad()) {
    throw ZException(fmt::format("Error while reading file {}", filename), ZException::Option::CheckErrno);
  }

  if (notEmptyLineIdx > 0) {
    mat.conservativeResize(notEmptyLineIdx, Eigen::NoChange);
  }
  return mat;
}

MatrixXd ZEigenUtils::removeRowsContainNaNOrInF(const Eigen::MatrixXd& srcMat)
{
  MatrixXd mat(srcMat.rows(), srcMat.cols());
  Eigen::Index idx = 0;
  for (Eigen::Index i = 0; i < srcMat.rows(); ++i) {
    if (srcMat.row(i).allFinite()) {
      mat.row(idx++) = srcMat.row(i);
    }
  }
  mat.conservativeResize(idx, Eigen::NoChange);
  return mat;
}

} // namespace nim
