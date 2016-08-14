#include "zeigenutils.h"

#include "zexception.h"
#include "zstringutils.h"
#include <QFile>

namespace nim {

using namespace Eigen;

MatrixXd ZEigenUtils::readMatrix(const QString& filename, const char* uSep, bool strictDelimiter, double fillValue,
                                 const std::string& commentStart)
{
  MatrixXd mat;
  MatrixXd emptyMat;

  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios::in);

  int nRow = std::count(std::istreambuf_iterator<char>(inputFileStream), std::istreambuf_iterator<char>(), '\n') + 1;
  inputFileStream.clear();
  inputFileStream.seekg(0, std::ios::beg);

  if (inputFileStream.bad()) {
    throw ZIOException(QString("Error while reading file %1").arg(filename));
  }

  int nCol = 0;
  int notEmptyLineIdx = 0;
  std::string line;
  while (std::getline(inputFileStream, line)) {
    if (nCol == 0) {  //get number of col and make space in mat
      readRowVector(line, uSep, &nCol, -1, strictDelimiter, fillValue, commentStart);
      if (nCol == 0)
        continue;
      mat.resize(nRow, nCol);
    }

    int nAData;
    RowVectorXd rowVector = readRowVector(line, uSep, &nAData, nCol, strictDelimiter, fillValue, commentStart);
    if (nAData == 0) {
      continue;
    } else {
      mat.row(notEmptyLineIdx++) = rowVector;
    }
  }

  if (inputFileStream.bad()) {
    throw ZIOException(QString("Error while reading file %1").arg(filename));
  }

  inputFileStream.close();
  if (notEmptyLineIdx > 0) {
    mat.conservativeResize(notEmptyLineIdx, Eigen::NoChange);
    return mat;
  } else {
    return emptyMat;
  }
}

RowVectorXd ZEigenUtils::readRowVector(const std::string& iline, const char* uSep, int* nActualData,
                                       int nReadData, bool strictDelimiter, double fillValue,
                                       const std::string& commentStart)
{
  RowVectorXd rowVector;
  std::string line = iline.substr(0, iline.find_first_of("\n"));  //extract only one line to process
  // remove comment
  removeComment(line, commentStart, true);
  int nData = 0;
  std::string sep;
  std::string possiblespace = " \t\r";
  if (strictDelimiter) {
    sep = uSep;
    CHECK(sep.size() == 1 && sep.find_first_not_of(possiblespace) != std::string::npos);
  } else {
    sep = ",:; \t\r[]";   // \r for windows file
    sep += uSep;
  }
  if (nActualData || nReadData == -1) {
    if (strictDelimiter) {
      size_t startPos = line.find_first_not_of(possiblespace);   //skip leading space
      if (startPos == std::string::npos) {
        nData = 0;
        if (nActualData)
          *nActualData = nData;
        return rowVector;
      }
      std::istringstream tmpStream;
      tmpStream.str(line);
      nData = std::count(std::istreambuf_iterator<char>(tmpStream), std::istreambuf_iterator<char>(), sep[0]) + 1;
    } else {
      size_t startPos = line.find_first_not_of(sep);   //skip leading space
      if (startPos == std::string::npos) {
        nData = 0;
        if (nActualData)
          *nActualData = nData;
        return rowVector;
      }
      do {
        size_t ePos = line.find_first_of(sep, startPos);  //start of separator
        if (ePos != std::string::npos) {  //skip consecutive separator
          ePos = line.find_first_not_of(sep, ePos);
        }
        nData++;
        startPos = ePos;
      } while (startPos != std::string::npos);
    }
  }

  if (nActualData)
    *nActualData = nData;

  if (nReadData == -1)
    nReadData = nData;
  rowVector.resize(nReadData);

  // read data
  bool hasData = true;
  size_t sPos;
  if (strictDelimiter) {
    sPos = line.find_first_not_of(possiblespace);
  } else {
    sPos = line.find_first_not_of(sep);   //skip spaces
  }
  for (int i = 0; i < nReadData; i++) {
    if (!hasData) {
      rowVector(i) = fillValue;
      continue;
    }
    size_t ePos = line.find_first_of(sep, sPos);
    std::string valString = line.substr(sPos, ePos - sPos);
    //convert valString to double
    if (strictDelimiter && valString.empty()) {
      rowVector(i) = fillValue;
    } else {
      std::transform(valString.begin(), valString.end(), valString.begin(), ::tolower);

      if (valString == "-inf" || valString == "-1.#inf") {
        rowVector(i) = -std::numeric_limits<double>::infinity();
      } else if (valString == "inf" || valString == "+inf" ||
                 valString == "1.#inf" || valString == "+1.#inf") {
        rowVector(i) = std::numeric_limits<double>::infinity();
      } else if (valString == "nan" || valString == "-nan" || valString == "+nan" ||
                 valString == "1.#qnan" || valString == "-1.#qnan" || valString == "+1.#qnan" ||
                 valString == "1.#ind" || valString == "-1.#ind" || valString == "+1.#ind" ||
                 valString == "1.#snan" || valString == "-1.#snan" || valString == "+1.#snan") {
        rowVector(i) = std::numeric_limits<double>::quiet_NaN();
      } else {
#if 0
        std::istringstream tmpStream;
        tmpStream.str(valString);
        tmpStream.clear();
        double val;
        tmpStream >> val;
        if (!tmpStream)
          rowVector(i) = std::numeric_limits<double>::quiet_NaN();
        else
          rowVector(i) = val;
#else
        double val = std::strtod(valString.c_str(), nullptr);
        rowVector(i) = (val == HUGE_VAL) ? std::numeric_limits<double>::infinity() :
                       (val == -HUGE_VAL) ? -std::numeric_limits<double>::infinity() : val;
#endif
      }
    }

    if (ePos == std::string::npos) {
      hasData = false;
      continue;
    }
    if (strictDelimiter) {
      sPos = ePos + 1;
    } else {
      sPos = line.find_first_not_of(sep, ePos); //skip seps
    }
    if (sPos >= line.size() || sPos == std::string::npos) {
      hasData = false;
      continue;
    }
  }
  return rowVector;
}

MatrixXd ZEigenUtils::removeRowsContainNaNOrInF(const Eigen::MatrixXd& srcMat)
{
  MatrixXd mat(srcMat.rows(), srcMat.cols());
  Eigen::Index idx = 0;
  for (Eigen::Index i = 0; i < srcMat.rows(); i++) {
    if (srcMat.row(i).allFinite()) {
      mat.row(idx++) = srcMat.row(i);
    }
  }
  mat.conservativeResize(idx, Eigen::NoChange);
  return mat;
}

} // namespace nim
