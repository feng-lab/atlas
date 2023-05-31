#include "zsaturateoperation_avx2.h"

#include "zsaturateoperation.h"
#include "zglobal.h"
#include "zlog.h"
#include <simde/x86/avx2.h>

namespace nim {

void saturate_add_avx2(const uint8_t* x, const uint8_t* y, size_t count, uint8_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(y, 32) && is_aligned(res, 32)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 32) {
    for (; i < count - 31; i += 32) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_adds_epu8(l, r));
    }
  } else if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = simde_mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_adds_epu8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y[i]);
  }
}

void saturate_add_avx2(const uint8_t* x, uint8_t y, size_t count, uint8_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(res, 32)) << x << " " << res;
  size_t i = 0;
  if (count >= 32) {
    auto r = simde_mm256_set1_epi8(y);
    for (; i < count - 31; i += 32) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_adds_epu8(l, r));
    }
  } else if (count >= 16) {
    auto r = simde_mm_set1_epi8(y);
    for (; i < count - 15; i += 16) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_adds_epu8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y);
  }
}

void saturate_add_avx2(const int8_t* x, const int8_t* y, size_t count, int8_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(y, 32) && is_aligned(res, 32)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 32) {
    for (; i < count - 31; i += 32) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_adds_epi8(l, r));
    }
  } else if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = simde_mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_adds_epi8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y[i]);
  }
}

void saturate_add_avx2(const int8_t* x, int8_t y, size_t count, int8_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(res, 32)) << x << " " << res;
  size_t i = 0;
  if (count >= 32) {
    auto r = simde_mm256_set1_epi8(y);
    for (; i < count - 31; i += 32) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_adds_epi8(l, r));
    }
  } else if (count >= 16) {
    auto r = simde_mm_set1_epi8(y);
    for (; i < count - 15; i += 16) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_adds_epi8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y);
  }
}

void saturate_add_avx2(const uint16_t* x, const uint16_t* y, size_t count, uint16_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(y, 32) && is_aligned(res, 32)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_adds_epu16(l, r));
    }
  } else if (count >= 8) {
    for (; i < count - 7; i += 8) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = simde_mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_adds_epu16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y[i]);
  }
}

void saturate_add_avx2(const uint16_t* x, uint16_t y, size_t count, uint16_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(res, 32)) << x << " " << res;
  size_t i = 0;
  if (count >= 16) {
    auto r = simde_mm256_set1_epi16(y);
    for (; i < count - 15; i += 16) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_adds_epu16(l, r));
    }
  } else if (count >= 8) {
    auto r = simde_mm_set1_epi16(y);
    for (; i < count - 7; i += 8) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_adds_epu16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y);
  }
}

void saturate_add_avx2(const int16_t* x, const int16_t* y, size_t count, int16_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(y, 32) && is_aligned(res, 32)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_adds_epi16(l, r));
    }
  } else if (count >= 8) {
    for (; i < count - 7; i += 8) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = simde_mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_adds_epi16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y[i]);
  }
}

void saturate_add_avx2(const int16_t* x, int16_t y, size_t count, int16_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(res, 32)) << x << " " << res;
  size_t i = 0;
  if (count >= 16) {
    auto r = simde_mm256_set1_epi16(y);
    for (; i < count - 15; i += 16) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_adds_epi16(l, r));
    }
  } else if (count >= 8) {
    auto r = simde_mm_set1_epi16(y);
    for (; i < count - 7; i += 8) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_adds_epi16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y);
  }
}

void saturate_sub_avx2(const uint8_t* x, const uint8_t* y, size_t count, uint8_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(y, 32) && is_aligned(res, 32)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 32) {
    for (; i < count - 31; i += 32) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_subs_epu8(l, r));
    }
  } else if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = simde_mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_subs_epu8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y[i]);
  }
}

void saturate_sub_avx2(const uint8_t* x, uint8_t y, size_t count, uint8_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(res, 32)) << x << " " << res;
  size_t i = 0;
  if (count >= 32) {
    auto r = simde_mm256_set1_epi8(y);
    for (; i < count - 31; i += 32) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_subs_epu8(l, r));
    }
  } else if (count >= 16) {
    auto r = simde_mm_set1_epi8(y);
    for (; i < count - 15; i += 16) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_subs_epu8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y);
  }
}

void saturate_sub_avx2(const int8_t* x, const int8_t* y, size_t count, int8_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(y, 32) && is_aligned(res, 32)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 32) {
    for (; i < count - 31; i += 32) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_subs_epi8(l, r));
    }
  } else if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = simde_mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_subs_epi8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y[i]);
  }
}

void saturate_sub_avx2(const int8_t* x, int8_t y, size_t count, int8_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(res, 32)) << x << " " << res;
  size_t i = 0;
  if (count >= 32) {
    auto r = simde_mm256_set1_epi8(y);
    for (; i < count - 31; i += 32) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_subs_epi8(l, r));
    }
  } else if (count >= 16) {
    auto r = simde_mm_set1_epi8(y);
    for (; i < count - 15; i += 16) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_subs_epi8(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y);
  }
}

void saturate_sub_avx2(const uint16_t* x, const uint16_t* y, size_t count, uint16_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(y, 32) && is_aligned(res, 32)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_subs_epu16(l, r));
    }
  } else if (count >= 8) {
    for (; i < count - 7; i += 8) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = simde_mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_subs_epu16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y[i]);
  }
}

void saturate_sub_avx2(const uint16_t* x, uint16_t y, size_t count, uint16_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(res, 32)) << x << " " << res;
  size_t i = 0;
  if (count >= 16) {
    auto r = simde_mm256_set1_epi16(y);
    for (; i < count - 15; i += 16) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_subs_epu16(l, r));
    }
  } else if (count >= 8) {
    auto r = simde_mm_set1_epi16(y);
    for (; i < count - 7; i += 8) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_subs_epu16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y);
  }
}

void saturate_sub_avx2(const int16_t* x, const int16_t* y, size_t count, int16_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(y, 32) && is_aligned(res, 32)) << x << " " << y << " " << res;
  size_t i = 0;
  if (count >= 16) {
    for (; i < count - 15; i += 16) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      auto r = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(y + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_subs_epi16(l, r));
    }
  } else if (count >= 8) {
    for (; i < count - 7; i += 8) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      auto r = simde_mm_load_si128(reinterpret_cast<const __m128i*>(y + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_subs_epi16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y[i]);
  }
}

void saturate_sub_avx2(const int16_t* x, int16_t y, size_t count, int16_t* res)
{
  CHECK(is_aligned(x, 32) && is_aligned(res, 32)) << x << " " << res;
  size_t i = 0;
  if (count >= 16) {
    auto r = simde_mm256_set1_epi16(y);
    for (; i < count - 15; i += 16) {
      auto l = simde_mm256_load_si256(reinterpret_cast<const __m256i*>(x + i));
      simde_mm256_store_si256(reinterpret_cast<__m256i*>(res + i), simde_mm256_subs_epi16(l, r));
    }
  } else if (count >= 8) {
    auto r = simde_mm_set1_epi16(y);
    for (; i < count - 7; i += 8) {
      auto l = simde_mm_load_si128(reinterpret_cast<const __m128i*>(x + i));
      simde_mm_store_si128(reinterpret_cast<__m128i*>(res + i), simde_mm_subs_epi16(l, r));
    }
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y);
  }
}

} // namespace nim
