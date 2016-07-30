#include "zimageavx.h"
#include <immintrin.h>

namespace nim {

void Image2DFilterForOneBlock_AVX(const double* padImg, size_t padImgWidth, const double* kernel, size_t kernelWidth,
                                  size_t kernelHeight, double* imgOut, size_t imgOutWidth, size_t rangeStart,
                                  size_t rangeEnd)
{
  for (size_t j = rangeStart; j < rangeEnd; ++j) {
    for (size_t i = 0; i < imgOutWidth; ++i) {
      double sum = 0.0;
      __m256d vsum = _mm256_set1_pd(0.0);
      //double sumTest = 0.0;
      for (size_t r = 0; r < kernelHeight; ++r) {  // row by row
        const double* imgStart = padImg + (j + r) * padImgWidth + i;

        //sumTest = std::inner_product(imgStart, imgStart+kernelWidth,
        //                        kernel+r*kernelWidth, sumTest);
        size_t k;
        // process 4 elements per iteration
        for (k = 0; k < kernelWidth - 3; k += 4) {
          __m256d va = _mm256_loadu_pd(imgStart + k);
          __m256d vb = _mm256_loadu_pd(kernel + r * kernelWidth + k);
          __m256d vs = _mm256_mul_pd(va, vb);
          vsum = _mm256_add_pd(vsum, vs);
        }

        // clean up any remaining elements
        for (; k < kernelWidth; ++k) {
          sum += imgStart[k] * (*(kernel + r * kernelWidth + k));
        }
      }
      // horizontal sum of 4 partial dot products
      vsum = _mm256_hadd_pd(_mm256_permute2f128_pd(vsum, vsum, 0x20), _mm256_permute2f128_pd(vsum, vsum, 0x31));
      vsum = _mm256_hadd_pd(_mm256_permute2f128_pd(vsum, vsum, 0x20), _mm256_permute2f128_pd(vsum, vsum, 0x31));
      double tmpSum[4];
      _mm256_storeu_pd(tmpSum, vsum);
      sum += tmpSum[0];
      //CHECK_LE(std::abs(sum-sumTest), std::numeric_limits<double>::epsilon()*100) << std::abs(sum-sumTest);
      imgOut[j * imgOutWidth + i] = sum;
    }
  }
}

void Image2DRowFilterForOneBlock_AVX(const double* padImg, size_t padImgWidth, const double* kernel, size_t kernelWidth,
                                     double* imgOut, size_t imgOutWidth, size_t rangeStart, size_t rangeEnd)
{
  for (size_t j = rangeStart; j < rangeEnd; ++j) {
    for (size_t i = 0; i < imgOutWidth; ++i) {
      double sum[4] = {0.0};
      __m256d vsum = _mm256_set1_pd(0.0);
      //double sumTest = 0.0;
      const double* imgStart = padImg + j * padImgWidth + i;

      //sumTest = std::inner_product(imgStart, imgStart+kernelWidth,
      //                        kernel+r*kernelWidth, sumTest);
      size_t k;
      // process 4 elements per iteration
      for (k = 0; k < kernelWidth - 3; k += 4) {
        __m256d va = _mm256_loadu_pd(imgStart + k);
        __m256d vb = _mm256_load_pd(kernel + k);
        __m256d vs = _mm256_mul_pd(va, vb);
        vsum = _mm256_add_pd(vsum, vs);
      }

      // horizontal sum of 4 partial dot products
      vsum = _mm256_hadd_pd(_mm256_permute2f128_pd(vsum, vsum, 0x20), _mm256_permute2f128_pd(vsum, vsum, 0x31));
      vsum = _mm256_hadd_pd(_mm256_permute2f128_pd(vsum, vsum, 0x20), _mm256_permute2f128_pd(vsum, vsum, 0x31));
      _mm256_storeu_pd(sum, vsum);

      // clean up any remaining elements
      for (; k < kernelWidth; ++k) {
        sum[0] += imgStart[k] * (*(kernel + k));
      }

      //CHECK_LE(std::abs(sum-sumTest), std::numeric_limits<double>::epsilon()*100) << std::abs(sum-sumTest);
      imgOut[j * imgOutWidth + i] = sum[0];
    }
  }
}

void Image3DFilterForOneBlock_AVX(const double* padImg, size_t padImgWidth, size_t padImgHeight, const double* kernel,
                                  size_t kernelWidth, size_t kernelHeight, size_t kernelDepth, double* imgOut,
                                  size_t imgOutWidth,
                                  size_t imgOutHeight, size_t rangeStart, size_t rangeEnd)
{
  for (size_t k = rangeStart; k < rangeEnd; ++k) {
    for (size_t j = 0; j < imgOutHeight; ++j) {
      for (size_t i = 0; i < imgOutWidth; ++i) {
        double sum = 0.0;
        __m256d vsum = _mm256_set1_pd(0.0);
        //double sumTest = 0.0;
        for (size_t s = 0; s < kernelDepth; ++s) { // plane by plane
          for (size_t r = 0; r < kernelHeight; ++r) {  // row by row
            const double* imgStart = padImg + (j + r) * padImgWidth + i + (s + k) * padImgWidth * padImgHeight;

            //sumTest = std::inner_product(imgStart, imgStart+kernelWidth,
            //                        kernel+r*kernelWidth+s*kernelWidth*kernelHeight, sumTest);
            size_t k0;
            // process 4 elements per iteration
            for (k0 = 0; k0 < kernelWidth - 3; k0 += 4) {
              __m256d va = _mm256_loadu_pd(imgStart + k0);
              __m256d vb = _mm256_loadu_pd(kernel + r * kernelWidth + k0 + s * kernelWidth * kernelHeight);
              __m256d vs = _mm256_mul_pd(va, vb);
              vsum = _mm256_add_pd(vsum, vs);
            }

            // clean up any remaining elements
            for (; k0 < kernelWidth; ++k0) {
              sum += imgStart[k0] * (*(kernel + r * kernelWidth + k0 + s * kernelWidth * kernelHeight));
            }
          }
        }
        // horizontal sum of 4 partial dot products
        vsum = _mm256_hadd_pd(_mm256_permute2f128_pd(vsum, vsum, 0x20), _mm256_permute2f128_pd(vsum, vsum, 0x31));
        vsum = _mm256_hadd_pd(_mm256_permute2f128_pd(vsum, vsum, 0x20), _mm256_permute2f128_pd(vsum, vsum, 0x31));
        double tmpSum[4];
        _mm256_storeu_pd(tmpSum, vsum);
        sum += tmpSum[0];
        //CHECK_LE(std::abs(sum-sumTest), std::numeric_limits<double>::epsilon()*100) << std::abs(sum-sumTest);
        imgOut[j * imgOutWidth + i + k * imgOutWidth * imgOutHeight] = sum;
      }
    }
  }
}

void
Image3DRowFilterForOneBlock_AVX(const double* padImg, size_t padImgWidth, size_t padImgHeight, const double* kernel,
                                size_t kernelWidth, double* imgOut, size_t imgOutWidth, size_t imgOutHeight,
                                size_t rangeStart, size_t rangeEnd)
{
  for (size_t k = rangeStart; k < rangeEnd; ++k) {
    for (size_t j = 0; j < imgOutHeight; ++j) {
      for (size_t i = 0; i < imgOutWidth; ++i) {
        double sum[4] = {0.0};
        __m256d vsum = _mm256_set1_pd(0.0);
        //double sumTest = 0.0;
        const double* imgStart = padImg + j * padImgWidth + i + k * padImgWidth * padImgHeight;

        //sumTest = std::inner_product(imgStart, imgStart+kernelWidth,
        //                        kernel, sumTest);
        size_t k0;
        // process 4 elements per iteration
        for (k0 = 0; k0 < kernelWidth - 3; k0 += 4) {
          __m256d va = _mm256_loadu_pd(imgStart + k0);
          __m256d vb = _mm256_load_pd(kernel + k0);
          __m256d vs = _mm256_mul_pd(va, vb);
          vsum = _mm256_add_pd(vsum, vs);
        }

        // horizontal sum of 4 partial dot products
        vsum = _mm256_hadd_pd(_mm256_permute2f128_pd(vsum, vsum, 0x20), _mm256_permute2f128_pd(vsum, vsum, 0x31));
        vsum = _mm256_hadd_pd(_mm256_permute2f128_pd(vsum, vsum, 0x20), _mm256_permute2f128_pd(vsum, vsum, 0x31));
        _mm256_storeu_pd(sum, vsum);

        // clean up any remaining elements
        for (; k0 < kernelWidth; ++k0) {
          sum[0] += imgStart[k0] * (*(kernel + k0));
        }

        //CHECK_LE(std::abs(sum-sumTest), std::numeric_limits<double>::epsilon()*100) << std::abs(sum-sumTest);
        imgOut[j * imgOutWidth + i + k * imgOutWidth * imgOutHeight] = sum[0];
      }
    }
  }
}

}
