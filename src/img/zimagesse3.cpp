#include "zimagesse3.h"

#include "zimagearch.h"
#include "zglobal.h"

#if ATLAS_IMG_ARCH_X86_64
#include <immintrin.h>

namespace {

__forceinline double hsum_double_sse2(__m128d vd)
{ // v = [ B | A ]
  auto undef = _mm_undefined_ps(); // only used as the ignored high input for movehl.
  auto shuftmp = _mm_movehl_ps(undef, _mm_castpd_ps(vd)); // there is no movhlpd
  auto shuf = _mm_castps_pd(shuftmp);
  return _mm_cvtsd_f64(_mm_add_sd(vd, shuf));
}

} // namespace

namespace nim {

void Image2DFilterForOneBlock_SSE3(const double* padImg,
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
      auto vsum = _mm_set1_pd(0.0);
      // double sumTest = 0.0;
      for (size_t r = 0; r < kernelHeight; ++r) { // row by row
        const double* imgStart = padImg + (j + r) * padImgWidth + i;

        // sumTest = std::inner_product(imgStart, imgStart+kernelWidth,
        //                         kernel+r*kernelWidth, sumTest);
        size_t k;
        // process 2 elements per iteration
        for (k = 0; k < kernelWidth - 1; k += 2) {
          auto va = _mm_loadu_pd(imgStart + k);
          auto vb = _mm_loadu_pd(kernel + r * kernelWidth + k);
          auto vs = _mm_mul_pd(va, vb);
          vsum = _mm_add_pd(vsum, vs);
        }

        // clean up any remaining elements
        for (; k < kernelWidth; ++k) {
          sum += imgStart[k] * (*(kernel + r * kernelWidth + k));
        }
      }
      // CHECK_LE(std::abs(sum-sumTest), std::numeric_limits<double>::epsilon()*100) << std::abs(sum-sumTest);
      imgOut[j * imgOutWidth + i] = sum + hsum_double_sse2(vsum);
    }
  }
}

void Image2DRowFilterForOneBlock_SSE3(const double* padImg,
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
      auto vsum = _mm_set1_pd(0.0);
      // double sumTest = 0.0;
      const double* imgStart = padImg + j * padImgWidth + i;

      // sumTest = std::inner_product(imgStart, imgStart+kernelWidth,
      //                         kernel+r*kernelWidth, sumTest);
      size_t k;
      // process 2 elements per iteration
      for (k = 0; k < kernelWidth - 1; k += 2) {
        auto va = _mm_loadu_pd(imgStart + k);
        auto vb = _mm_load_pd(kernel + k);
        auto vs = _mm_mul_pd(va, vb);
        vsum = _mm_add_pd(vsum, vs);
      }

      // clean up any remaining elements
      for (; k < kernelWidth; ++k) {
        sum += imgStart[k] * (*(kernel + k));
      }

      // CHECK_LE(std::abs(sum-sumTest), std::numeric_limits<double>::epsilon()*100) << std::abs(sum-sumTest);
      imgOut[j * imgOutWidth + i] = sum + hsum_double_sse2(vsum);
    }
  }
}

void Image3DFilterForOneBlock_SSE3(const double* padImg,
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
        auto vsum = _mm_set1_pd(0.0);
        // double sumTest = 0.0;
        for (size_t s = 0; s < kernelDepth; ++s) { // plane by plane
          for (size_t r = 0; r < kernelHeight; ++r) { // row by row
            const double* imgStart = padImg + (j + r) * padImgWidth + i + (s + k) * padImgWidth * padImgHeight;

            // sumTest = std::inner_product(imgStart, imgStart+kernelWidth,
            //                         kernel+r*kernelWidth+s*kernelWidth*kernelHeight, sumTest);
            size_t k0;
            // process 2 elements per iteration
            for (k0 = 0; k0 < kernelWidth - 1; k0 += 2) {
              auto va = _mm_loadu_pd(imgStart + k0);
              auto vb = _mm_loadu_pd(kernel + r * kernelWidth + k0 + s * kernelWidth * kernelHeight);
              auto vs = _mm_mul_pd(va, vb);
              vsum = _mm_add_pd(vsum, vs);
            }

            // clean up any remaining elements
            for (; k0 < kernelWidth; ++k0) {
              sum += imgStart[k0] * (*(kernel + r * kernelWidth + k0 + s * kernelWidth * kernelHeight));
            }
          }
        }
        // CHECK_LE(std::abs(sum-sumTest), std::numeric_limits<double>::epsilon()*100) << std::abs(sum-sumTest);
        imgOut[j * imgOutWidth + i + k * imgOutWidth * imgOutHeight] = sum + hsum_double_sse2(vsum);
      }
    }
  }
}

void Image3DRowFilterForOneBlock_SSE3(const double* padImg,
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
        auto vsum = _mm_set1_pd(0.0);
        // double sumTest = 0.0;
        const double* imgStart = padImg + j * padImgWidth + i + k * padImgWidth * padImgHeight;

        // sumTest = std::inner_product(imgStart, imgStart+kernelWidth,
        //                              kernel, sumTest);
        size_t k0;
        // process 2 elements per iteration
        for (k0 = 0; k0 < kernelWidth - 1; k0 += 2) {
          auto va = _mm_loadu_pd(imgStart + k0);
          auto vb = _mm_load_pd(kernel + k0);
          auto vs = _mm_mul_pd(va, vb);
          vsum = _mm_add_pd(vsum, vs);
        }

        // clean up any remaining elements
        for (; k0 < kernelWidth; ++k0) {
          sum += imgStart[k0] * (*(kernel + k0));
        }

        // CHECK_LE(std::abs(sum-sumTest), std::numeric_limits<double>::epsilon()*100) << std::abs(sum-sumTest);
        imgOut[j * imgOutWidth + i + k * imgOutWidth * imgOutHeight] = sum + hsum_double_sse2(vsum);
      }
    }
  }
}

} // namespace nim

#endif // ATLAS_IMG_ARCH_X86_64
