#include "zimageavx.h"

#include "zglobal.h"
#ifdef _MSC_VER
#include <cmath> // for simde
#endif
#include <simde/x86/avx.h>

namespace {

__forceinline double hsum_double_avx(__m256d v)
{
  auto vlow = _mm256_castpd256_pd128(v);
  auto vhigh = _mm256_extractf128_pd(v, 1); // high 128
  vlow = _mm_add_pd(vlow, vhigh); // reduce down to 128

  auto high64 = _mm_unpackhi_pd(vlow, vlow);
  return _mm_cvtsd_f64(_mm_add_sd(vlow, high64)); // reduce to scalar
}

__forceinline float hsum_float_sse3(__m128 v)
{
  auto shuf = _mm_movehdup_ps(v); // broadcast elements 3,1 to 2,0
  auto sums = _mm_add_ps(v, shuf);
  shuf = _mm_movehl_ps(shuf, sums); // high half -> low half
  sums = _mm_add_ss(sums, shuf);
  return _mm_cvtss_f32(sums);
}

[[maybe_unused]] __forceinline float hsum256_float_avx(__m256 v)
{
  auto vlow = _mm256_castps256_ps128(v);
  auto vhigh = _mm256_extractf128_ps(v, 1); // high 128
  vlow = _mm_add_ps(vlow, vhigh); // add the low 128
  return hsum_float_sse3(vlow); // and inline the sse3 version, which is optimal for AVX
  // (no wasted instructions, and all of them are the 4B minimum)
}

} // namespace

namespace nim {

void Image2DFilterForOneBlock_AVX(const double* padImg,
                                  size_t padImgWidth,
                                  const double* kernel,
                                  size_t kernelWidth,
                                  size_t kernelHeight,
                                  double* imgOut,
                                  size_t imgOutWidth,
                                  size_t rangeStart,
                                  size_t rangeEnd)
{
  for (size_t j = rangeStart; j < rangeEnd; ++j) {
    for (size_t i = 0; i < imgOutWidth; ++i) {
      double sum = 0.0;
      auto vsum = _mm256_set1_pd(0.0);
      // double sumTest = 0.0;
      for (size_t r = 0; r < kernelHeight; ++r) { // row by row
        const double* imgStart = padImg + (j + r) * padImgWidth + i;

        // sumTest = std::inner_product(imgStart, imgStart+kernelWidth,
        //                         kernel+r*kernelWidth, sumTest);
        size_t k;
        // process 4 elements per iteration
        for (k = 0; k < kernelWidth - 3; k += 4) {
          auto va = _mm256_loadu_pd(imgStart + k);
          auto vb = _mm256_loadu_pd(kernel + r * kernelWidth + k);
          auto vs = _mm256_mul_pd(va, vb);
          vsum = _mm256_add_pd(vsum, vs);
        }

        // clean up any remaining elements
        for (; k < kernelWidth; ++k) {
          sum += imgStart[k] * (*(kernel + r * kernelWidth + k));
        }
      }
      // CHECK_LE(std::abs(sum-sumTest), std::numeric_limits<double>::epsilon()*100) << std::abs(sum-sumTest);
      imgOut[j * imgOutWidth + i] = sum + hsum_double_avx(vsum);
    }
  }
}

void Image2DRowFilterForOneBlock_AVX(const double* padImg,
                                     size_t padImgWidth,
                                     const double* kernel,
                                     size_t kernelWidth,
                                     double* imgOut,
                                     size_t imgOutWidth,
                                     size_t rangeStart,
                                     size_t rangeEnd)
{
  for (size_t j = rangeStart; j < rangeEnd; ++j) {
    for (size_t i = 0; i < imgOutWidth; ++i) {
      double sum = 0.0;
      auto vsum = _mm256_set1_pd(0.0);
      // double sumTest = 0.0;
      const double* imgStart = padImg + j * padImgWidth + i;

      // sumTest = std::inner_product(imgStart, imgStart+kernelWidth,
      //                         kernel+r*kernelWidth, sumTest);
      size_t k;
      // process 4 elements per iteration
      for (k = 0; k < kernelWidth - 3; k += 4) {
        auto va = _mm256_loadu_pd(imgStart + k);
        auto vb = _mm256_load_pd(kernel + k);
        auto vs = _mm256_mul_pd(va, vb);
        vsum = _mm256_add_pd(vsum, vs);
      }

      // clean up any remaining elements
      for (; k < kernelWidth; ++k) {
        sum += imgStart[k] * (*(kernel + k));
      }

      // CHECK_LE(std::abs(sum-sumTest), std::numeric_limits<double>::epsilon()*100) << std::abs(sum-sumTest);
      imgOut[j * imgOutWidth + i] = sum + hsum_double_avx(vsum);
    }
  }
}

void Image3DFilterForOneBlock_AVX(const double* padImg,
                                  size_t padImgWidth,
                                  size_t padImgHeight,
                                  const double* kernel,
                                  size_t kernelWidth,
                                  size_t kernelHeight,
                                  size_t kernelDepth,
                                  double* imgOut,
                                  size_t imgOutWidth,
                                  size_t imgOutHeight,
                                  size_t rangeStart,
                                  size_t rangeEnd)
{
  for (size_t k = rangeStart; k < rangeEnd; ++k) {
    for (size_t j = 0; j < imgOutHeight; ++j) {
      for (size_t i = 0; i < imgOutWidth; ++i) {
        double sum = 0.0;
        auto vsum = _mm256_set1_pd(0.0);
        // double sumTest = 0.0;
        for (size_t s = 0; s < kernelDepth; ++s) { // plane by plane
          for (size_t r = 0; r < kernelHeight; ++r) { // row by row
            const double* imgStart = padImg + (j + r) * padImgWidth + i + (s + k) * padImgWidth * padImgHeight;

            // sumTest = std::inner_product(imgStart, imgStart+kernelWidth,
            //                         kernel+r*kernelWidth+s*kernelWidth*kernelHeight, sumTest);
            size_t k0;
            // process 4 elements per iteration
            for (k0 = 0; k0 < kernelWidth - 3; k0 += 4) {
              auto va = _mm256_loadu_pd(imgStart + k0);
              auto vb = _mm256_loadu_pd(kernel + r * kernelWidth + k0 + s * kernelWidth * kernelHeight);
              auto vs = _mm256_mul_pd(va, vb);
              vsum = _mm256_add_pd(vsum, vs);
            }

            // clean up any remaining elements
            for (; k0 < kernelWidth; ++k0) {
              sum += imgStart[k0] * (*(kernel + r * kernelWidth + k0 + s * kernelWidth * kernelHeight));
            }
          }
        }
        // CHECK_LE(std::abs(sum-sumTest), std::numeric_limits<double>::epsilon()*100) << std::abs(sum-sumTest);
        imgOut[j * imgOutWidth + i + k * imgOutWidth * imgOutHeight] = sum + hsum_double_avx(vsum);
      }
    }
  }
}

void Image3DRowFilterForOneBlock_AVX(const double* padImg,
                                     size_t padImgWidth,
                                     size_t padImgHeight,
                                     const double* kernel,
                                     size_t kernelWidth,
                                     double* imgOut,
                                     size_t imgOutWidth,
                                     size_t imgOutHeight,
                                     size_t rangeStart,
                                     size_t rangeEnd)
{
  for (size_t k = rangeStart; k < rangeEnd; ++k) {
    for (size_t j = 0; j < imgOutHeight; ++j) {
      for (size_t i = 0; i < imgOutWidth; ++i) {
        double sum = 0.0;
        auto vsum = _mm256_set1_pd(0.0);
        // double sumTest = 0.0;
        const double* imgStart = padImg + j * padImgWidth + i + k * padImgWidth * padImgHeight;

        // sumTest = std::inner_product(imgStart, imgStart+kernelWidth,
        //                         kernel, sumTest);
        size_t k0;
        // process 4 elements per iteration
        for (k0 = 0; k0 < kernelWidth - 3; k0 += 4) {
          auto va = _mm256_loadu_pd(imgStart + k0);
          auto vb = _mm256_load_pd(kernel + k0);
          auto vs = _mm256_mul_pd(va, vb);
          vsum = _mm256_add_pd(vsum, vs);
        }

        // clean up any remaining elements
        for (; k0 < kernelWidth; ++k0) {
          sum += imgStart[k0] * (*(kernel + k0));
        }

        // CHECK_LE(std::abs(sum-sumTest), std::numeric_limits<double>::epsilon()*100) << std::abs(sum-sumTest);
        imgOut[j * imgOutWidth + i + k * imgOutWidth * imgOutHeight] = sum + hsum_double_avx(vsum);
      }
    }
  }
}

} // namespace nim
