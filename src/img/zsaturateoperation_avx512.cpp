#include "zsaturateoperation_avx512.h"

#include "zsaturateoperation.h"
#include "zglobal.h"
#include "zlog.h"
#include <cpuinfo.h>
#include <folly/CPortability.h>
#include <simde/x86/avx512.h>

namespace nim {

FOLLY_PUSH_WARNING
FOLLY_CLANG_DISABLE_WARNING("-Wunused-parameter")

void saturate_add_avx512(const uint8_t* x, const uint8_t* y, size_t count, uint8_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(y, 64) && is_aligned(res, 64)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 64) {
    for (; i < count - 63; i += 64) {
      auto l = _mm512_load_si512(x + i);
      auto r = _mm512_load_si512(y + i);
      _mm512_store_si512(res + i, _mm512_adds_epu8(l, r));
    }
  } else if (count >= 32) {
    for (; i < count - 31; i += 32) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = _mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_adds_epu8(l, r));
    }
  } else if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = _mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epu8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y[i]);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_add_avx512(const uint8_t* x, uint8_t y, size_t count, uint8_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(res, 64)) << x << " " << res;
  size_t i = 0;
  if (count >= 64) {
    auto r = _mm512_set1_epi8(y);
    for (; i < count - 63; i += 64) {
      auto l = _mm512_load_si512(x + i);
      _mm512_store_si512(res + i, _mm512_adds_epu8(l, r));
    }
  } else if (count >= 32) {
    auto r = _mm256_set1_epi8(y);
    for (; i < count - 31; i += 32) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_adds_epu8(l, r));
    }
  } else if (count >= 16) {
    auto r = _mm_set1_epi8(y);
    for (; i < count - 15; i += 16) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epu8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_add_avx512(const int8_t* x, const int8_t* y, size_t count, int8_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(y, 64) && is_aligned(res, 64)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 64) {
    for (; i < count - 63; i += 64) {
      auto l = _mm512_load_si512(x + i);
      auto r = _mm512_load_si512(y + i);
      _mm512_store_si512(res + i, _mm512_adds_epi8(l, r));
    }
  } else if (count >= 32) {
    for (; i < count - 31; i += 32) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = _mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_adds_epi8(l, r));
    }
  } else if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = _mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epi8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y[i]);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_add_avx512(const int8_t* x, int8_t y, size_t count, int8_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(res, 64)) << x << " " << res;
  size_t i = 0;
  if (count >= 64) {
    auto r = _mm512_set1_epi8(y);
    for (; i < count - 63; i += 64) {
      auto l = _mm512_load_si512(x + i);
      _mm512_store_si512(res + i, _mm512_adds_epi8(l, r));
    }
  } else if (count >= 32) {
    auto r = _mm256_set1_epi8(y);
    for (; i < count - 31; i += 32) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_adds_epi8(l, r));
    }
  } else if (count >= 16) {
    auto r = _mm_set1_epi8(y);
    for (; i < count - 15; i += 16) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epi8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_add_avx512(const uint16_t* x, const uint16_t* y, size_t count, uint16_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(y, 64) && is_aligned(res, 64)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 32) {
    for (; i < count - 31; i += 32) {
      auto l = _mm512_load_si512(x + i);
      auto r = _mm512_load_si512(y + i);
      _mm512_store_si512(res + i, _mm512_adds_epu16(l, r));
    }
  } else if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = _mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_adds_epu16(l, r));
    }
  } else if (count >= 8) {
    for (; i < count - 7; i += 8) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = _mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epu16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y[i]);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_add_avx512(const uint16_t* x, uint16_t y, size_t count, uint16_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(res, 64)) << x << " " << res;
  size_t i = 0;
  if (count >= 32) {
    auto r = _mm512_set1_epi16(y);
    for (; i < count - 31; i += 32) {
      auto l = _mm512_load_si512(x + i);
      _mm512_store_si512(res + i, _mm512_adds_epu16(l, r));
    }
  } else if (count >= 16) {
    auto r = _mm256_set1_epi16(y);
    for (; i < count - 15; i += 16) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_adds_epu16(l, r));
    }
  } else if (count >= 8) {
    auto r = _mm_set1_epi16(y);
    for (; i < count - 7; i += 8) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epu16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_add_avx512(const int16_t* x, const int16_t* y, size_t count, int16_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(y, 64) && is_aligned(res, 64)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 32) {
    for (; i < count - 31; i += 32) {
      auto l = _mm512_load_si512(x + i);
      auto r = _mm512_load_si512(y + i);
      _mm512_store_si512(res + i, _mm512_adds_epi16(l, r));
    }
  } else if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = _mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_adds_epi16(l, r));
    }
  } else if (count >= 8) {
    for (; i < count - 7; i += 8) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = _mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epi16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y[i]);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_add_avx512(const int16_t* x, int16_t y, size_t count, int16_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(res, 64)) << x << " " << res;
  size_t i = 0;
  if (count >= 32) {
    auto r = _mm512_set1_epi16(y);
    for (; i < count - 31; i += 32) {
      auto l = _mm512_load_si512(x + i);
      _mm512_store_si512(res + i, _mm512_adds_epi16(l, r));
    }
  } else if (count >= 16) {
    auto r = _mm256_set1_epi16(y);
    for (; i < count - 15; i += 16) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_adds_epi16(l, r));
    }
  } else if (count >= 8) {
    auto r = _mm_set1_epi16(y);
    for (; i < count - 7; i += 8) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epi16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_sub_avx512(const uint8_t* x, const uint8_t* y, size_t count, uint8_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(y, 64) && is_aligned(res, 64)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 64) {
    for (; i < count - 63; i += 64) {
      auto l = _mm512_load_si512(x + i);
      auto r = _mm512_load_si512(y + i);
      _mm512_store_si512(res + i, _mm512_subs_epu8(l, r));
    }
  } else if (count >= 32) {
    for (; i < count - 31; i += 32) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = _mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_subs_epu8(l, r));
    }
  } else if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = _mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epu8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y[i]);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_sub_avx512(const uint8_t* x, uint8_t y, size_t count, uint8_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(res, 64)) << x << " " << res;
  size_t i = 0;
  if (count >= 64) {
    auto r = _mm512_set1_epi8(y);
    for (; i < count - 63; i += 64) {
      auto l = _mm512_load_si512(x + i);
      _mm512_store_si512(res + i, _mm512_subs_epu8(l, r));
    }
  } else if (count >= 32) {
    auto r = _mm256_set1_epi8(y);
    for (; i < count - 31; i += 32) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_subs_epu8(l, r));
    }
  } else if (count >= 16) {
    auto r = _mm_set1_epi8(y);
    for (; i < count - 15; i += 16) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epu8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_sub_avx512(const int8_t* x, const int8_t* y, size_t count, int8_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(y, 64) && is_aligned(res, 64)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 64) {
    for (; i < count - 63; i += 64) {
      auto l = _mm512_load_si512(x + i);
      auto r = _mm512_load_si512(y + i);
      _mm512_store_si512(res + i, _mm512_subs_epi8(l, r));
    }
  } else if (count >= 32) {
    for (; i < count - 31; i += 32) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = _mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_subs_epi8(l, r));
    }
  } else if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = _mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epi8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y[i]);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_sub_avx512(const int8_t* x, int8_t y, size_t count, int8_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(res, 64)) << x << " " << res;
  size_t i = 0;
  if (count >= 64) {
    auto r = _mm512_set1_epi8(y);
    for (; i < count - 63; i += 64) {
      auto l = _mm512_load_si512(x + i);
      _mm512_store_si512(res + i, _mm512_subs_epi8(l, r));
    }
  } else if (count >= 32) {
    auto r = _mm256_set1_epi8(y);
    for (; i < count - 31; i += 32) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_subs_epi8(l, r));
    }
  } else if (count >= 16) {
    auto r = _mm_set1_epi8(y);
    for (; i < count - 15; i += 16) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epi8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_sub_avx512(const uint16_t* x, const uint16_t* y, size_t count, uint16_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(y, 64) && is_aligned(res, 64)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 32) {
    for (; i < count - 31; i += 32) {
      auto l = _mm512_load_si512(x + i);
      auto r = _mm512_load_si512(y + i);
      _mm512_store_si512(res + i, _mm512_subs_epu16(l, r));
    }
  } else if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = _mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_subs_epu16(l, r));
    }
  } else if (count >= 8) {
    for (; i < count - 7; i += 8) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = _mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epu16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y[i]);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_sub_avx512(const uint16_t* x, uint16_t y, size_t count, uint16_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(res, 64)) << x << " " << res;
  size_t i = 0;
  if (count >= 32) {
    auto r = _mm512_set1_epi16(y);
    for (; i < count - 31; i += 32) {
      auto l = _mm512_load_si512(x + i);
      _mm512_store_si512(res + i, _mm512_subs_epu16(l, r));
    }
  } else if (count >= 16) {
    auto r = _mm256_set1_epi16(y);
    for (; i < count - 15; i += 16) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_subs_epu16(l, r));
    }
  } else if (count >= 8) {
    auto r = _mm_set1_epi16(y);
    for (; i < count - 7; i += 8) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epu16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_sub_avx512(const int16_t* x, const int16_t* y, size_t count, int16_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(y, 64) && is_aligned(res, 64)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 32) {
    for (; i < count - 31; i += 32) {
      auto l = _mm512_load_si512(x + i);
      auto r = _mm512_load_si512(y + i);
      _mm512_store_si512(res + i, _mm512_subs_epi16(l, r));
    }
  } else if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = _mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_subs_epi16(l, r));
    }
  } else if (count >= 8) {
    for (; i < count - 7; i += 8) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = _mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epi16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y[i]);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

void saturate_sub_avx512(const int16_t* x, int16_t y, size_t count, int16_t* res)
{
#if CPUINFO_ARCH_X86_64
  CHECK(is_aligned(x, 64) && is_aligned(res, 64)) << x << " " << res;
  size_t i = 0;
  if (count >= 32) {
    auto r = _mm512_set1_epi16(y);
    for (; i < count - 31; i += 32) {
      auto l = _mm512_load_si512(x + i);
      _mm512_store_si512(res + i, _mm512_subs_epi16(l, r));
    }
  } else if (count >= 16) {
    auto r = _mm256_set1_epi16(y);
    for (; i < count - 15; i += 16) {
      auto l = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      _mm256_store_si256(reinterpret_cast<__m256i*>(res + i), _mm256_subs_epi16(l, r));
    }
  } else if (count >= 8) {
    auto r = _mm_set1_epi16(y);
    for (; i < count - 7; i += 8) {
      auto l = _mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      _mm_store_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epi16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y);
  }
#else
  LOG(FATAL) << "avx512 only supports x86_64";
#endif
}

FOLLY_POP_WARNING

} // namespace nim
