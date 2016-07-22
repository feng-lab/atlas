#include "zimage2dutils.h"
#include "zeigenutils.h"

using namespace Eigen;

namespace {

__forceinline double nearest_kernel(double x)
{
  return (-0.5 <= x) && (x < 0.5);
}

__forceinline double linear_kernel(double x)
{
  return (x+1) * ((-1 <= x) && (x < 0)) + (1-x) * ((0 <= x) && (x <= 1));
}

__forceinline double cubic_kernel(double x)
{
  double absx = std::abs(x);
  double absx2 = absx * absx;
  double absx3 = absx2 * absx;

  return (1.5*absx3 - 2.5*absx2 + 1) * (absx <= 1) +
      (-0.5*absx3 + 2.5*absx2 - 4*absx + 2) *
      ((1 < absx) && (absx <= 2));
}

__forceinline double lanczos2_kernel(double x)
{
  return (std::abs(x) < 2) * (std::sin(M_PI*x) * std::sin(M_PI*x/2) + std::numeric_limits<double>::epsilon()) /
      ((M_PI*M_PI * x*x / 2) + std::numeric_limits<double>::epsilon());
}

__forceinline double lanczos3_kernel(double x)
{
  return (std::abs(x) < 3) * (std::sin(M_PI*x) * std::sin(M_PI*x/3) + std::numeric_limits<double>::epsilon()) /
      ((M_PI*M_PI * x*x / 3) + std::numeric_limits<double>::epsilon());
}

}

namespace nim {

bool seperate2DKernel(const double *kernel, size_t width, size_t height,
                      double *rowKernel, double *colKernel)
{
  if (height == 1) {
    memcpy(rowKernel, kernel, sizeof(double)*width);
    colKernel[0] = 1;
    return true;
  }
  if (width == 1) {
    memcpy(colKernel, kernel, sizeof(double)*height);
    rowKernel[0] = 1;
    return true;
  }
  if (width > 1 && height > 1 && width*height >= 49) {
    Map<const Matrix<double,Dynamic,Dynamic,RowMajor>> knl(kernel,height,width);
    JacobiSVD<Matrix<double,Dynamic,Dynamic,RowMajor>> svd(knl, ComputeThinU | ComputeThinV);
    VectorXd s = svd.singularValues();
    //LINFO() << s;
    double tol = std::numeric_limits<double>::epsilon() * s(0) * std::max(width, height);
    int rank = 1;
    for (int i=1; i<s.size(); ++i)
      if (s(i) > tol)
        ++rank;
    if (rank == 1) {
      //LINFO() << "rank" << rank;
      //LINFO() << svd.matrixU();
      //LINFO() << svd.matrixV();
      Map<VectorXd>(colKernel, height) = svd.matrixU().col(0) * sqrt(s(0));
      Map<VectorXd>(rowKernel, width) = svd.matrixV().col(0) * sqrt(s(0));
      //      std::vector<double> show;
      //      show.insert(show.end(),rowKernel, rowKernel+width);
      //      logContainer(INFO, show, width);
      return true;
    } else {
      return false;
    }
  }
  return false;
}

template<>
void wrapCoordToImage<size_t>(size_t* coord, const size_t* imgSize, size_t numDimensions, PadOption padOption)
{
  if (padOption == PadOption::Symmetric) {
    for (size_t i=0; i<numDimensions; ++i) {
      if ((coord[i]/imgSize[i]) % 2 == 0)
        coord[i] = coord[i] % imgSize[i];
      else
        coord[i] = imgSize[i] - 1 - coord[i] % imgSize[i];
    }
  } else if (padOption == PadOption::Replicate) {
    for (size_t i=0; i<numDimensions; ++i) {
      if (coord[i] + 1 > imgSize[i])
        coord[i] = imgSize[i] - 1;
    }
  } else if (padOption == PadOption::Circular) {
    for (size_t i=0; i<numDimensions; ++i) {
      coord[i] = coord[i] % imgSize[i];
    }
  }
}

void _resizeContributions(size_t inLength, size_t outLength, Interpolant interpolant, bool antialiasing,
                          std::vector<double> &weightsOut, std::vector<size_t> &indicesOut, size_t &kernelWidthOut)
{
  assert(outLength > 0 && inLength > 0);
  double scale = double(outLength) / inLength;
  double kernelWidth = 0;
  switch (interpolant) {
  case Interpolant::Nearest:
    kernelWidth = 1;
    break;
  case Interpolant::Linear:
    kernelWidth = 2;
    break;
  case Interpolant::Cubic:
    kernelWidth = 4;
    break;
  case Interpolant::Lanczos2:
    kernelWidth = 4;
    break;
  case Interpolant::Lanczos3:
    kernelWidth = 6;
    break;
  default:
    assert(false);
    break;
  }
  if (scale < 1 && antialiasing) {
    kernelWidth /= scale;
  }

  double invScale = 1.0 / scale;
  ArrayXd inCoord(outLength);
  ArrayXXd indices(outLength, static_cast<size_t>(std::ceil(kernelWidth) + 2));
  typedef std::ptrdiff_t Index;
  for (Index i=0; i<inCoord.size(); ++i) {
    inCoord(i) = i * invScale - 0.5 * (1 - invScale);
    indices(i,0) = std::floor(inCoord(i) - 0.5 * kernelWidth);
  }
  for (Index i=1; i<indices.cols(); ++i) {
    indices.col(i) = indices.col(i-1) + 1;
  }

  //LINFO() << "incoord" << inCoord;
  //LINFO() << "indices" << indices;

  ArrayXXd weights = -indices;
  weights.colwise() += inCoord;

  switch (interpolant) {
  case Interpolant::Nearest:
    if (scale < 1 && antialiasing) {
      weights = weights.unaryExpr( [scale](double x)-> double { return scale * nearest_kernel(x * scale); } );
    } else {
      weights = weights.unaryExpr( [](double x)-> double { return nearest_kernel(x); } );
    }
    break;
  case Interpolant::Linear:
    if (scale < 1 && antialiasing) {
      weights = weights.unaryExpr( [scale](double x)-> double { return scale * linear_kernel(x * scale); } );
    } else {
      weights = weights.unaryExpr( [](double x)-> double { return linear_kernel(x); } );
    }
    break;
  case Interpolant::Cubic:
    if (scale < 1 && antialiasing) {
      weights = weights.unaryExpr( [scale](double x)-> double { return scale * cubic_kernel(x * scale); } );
    } else {
      weights = weights.unaryExpr( [](double x)-> double { return cubic_kernel(x); } );
    }
    break;
  case Interpolant::Lanczos2:
    if (scale < 1 && antialiasing) {
      weights = weights.unaryExpr( [scale](double x)-> double { return scale * lanczos2_kernel(x * scale); } );
    } else {
      weights = weights.unaryExpr( [](double x)-> double { return lanczos2_kernel(x); } );
    }
    break;
  case Interpolant::Lanczos3:
    if (scale < 1 && antialiasing) {
      weights = weights.unaryExpr( [scale](double x)-> double { return scale * lanczos3_kernel(x * scale); } );
    } else {
      weights = weights.unaryExpr( [](double x)-> double { return lanczos3_kernel(x); } );
    }
    break;
  default:
    assert(false);
    break;
  }

  weights.colwise() /= weights.rowwise().sum();

  //LINFO() << "weights" << weights;
  //LINFO() << "indices" << indices;

  auto validCols = (weights != 0).colwise().any();

  kernelWidthOut = validCols.count();
  weightsOut.resize(kernelWidthOut * outLength);
  indicesOut.resize(weightsOut.size());
  size_t idx = 0;

  for (Index r=0; r<weights.rows(); ++r) {
    for (Index c=0; c<weights.cols(); ++c) {
      if (validCols(c)) {
        weightsOut[idx] = weights(r,c);
        indicesOut[idx++] = std::max<int64_t>(0, std::min<int64_t>(inLength-1, indices(r,c)));
      }
    }
  }
  assert(idx == weightsOut.size());

  //logContainer(QsLogging::InfoLevel, weightsOut.begin(), weightsOut.end(), kernelWidthOut, "weights");
  //logContainer(QsLogging::InfoLevel, indicesOut.begin(), indicesOut.end(), kernelWidthOut, "indices");
}

} // namespace nim
