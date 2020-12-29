#include "zsaturateoperation.h"

#include "zcpuinfo.h"
#include "zlog.h"
#include "zsaturateoperation_avx2.h"
#include "zsaturateoperation_avx512.h"
#include <immintrin.h>

namespace nim {

template<>
void saturate_add<uint8_t, const uint8_t>(const uint8_t* x, const uint8_t* y, size_t count, uint8_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_add_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_add_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 16) {
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
}

template<>
void saturate_add<uint8_t, uint8_t>(const uint8_t* x, uint8_t y, size_t count, uint8_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_add_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_add_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 16) {
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
}

template<>
void saturate_add<int8_t, const int8_t>(const int8_t* x, const int8_t* y, size_t count, int8_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_add_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_add_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 16) {
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
}

template<>
void saturate_add<int8_t, int8_t>(const int8_t* x, int8_t y, size_t count, int8_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_add_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_add_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 16) {
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
}

template<>
void saturate_add<uint16_t, const uint16_t>(const uint16_t* x, const uint16_t* y, size_t count, uint16_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_add_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_add_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 8) {
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
}

template<>
void saturate_add<uint16_t, uint16_t>(const uint16_t* x, uint16_t y, size_t count, uint16_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_add_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_add_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 8) {
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
}

template<>
void saturate_add<int16_t, const int16_t>(const int16_t* x, const int16_t* y, size_t count, int16_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_add_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_add_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 8) {
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
}

template<>
void saturate_add<int16_t, int16_t>(const int16_t* x, int16_t y, size_t count, int16_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_add_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_add_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 8) {
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
}

template<>
void saturate_sub<uint8_t, const uint8_t>(const uint8_t* x, const uint8_t* y, size_t count, uint8_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_sub_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_sub_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 16) {
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
}

template<>
void saturate_sub<uint8_t, uint8_t>(const uint8_t* x, uint8_t y, size_t count, uint8_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_sub_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_sub_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 16) {
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
}

template<>
void saturate_sub<int8_t, const int8_t>(const int8_t* x, const int8_t* y, size_t count, int8_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_sub_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_sub_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 16) {
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
}

template<>
void saturate_sub<int8_t, int8_t>(const int8_t* x, int8_t y, size_t count, int8_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_sub_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_sub_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 16) {
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
}

template<>
void saturate_sub<uint16_t, const uint16_t>(const uint16_t* x, const uint16_t* y, size_t count, uint16_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_sub_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_sub_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 8) {
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
}

template<>
void saturate_sub<uint16_t, uint16_t>(const uint16_t* x, uint16_t y, size_t count, uint16_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_sub_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_sub_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 8) {
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
}

template<>
void saturate_sub<int16_t, const int16_t>(const int16_t* x, const int16_t* y, size_t count, int16_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_sub_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_sub_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 8) {
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
}

template<>
void saturate_sub<int16_t, int16_t>(const int16_t* x, int16_t y, size_t count, int16_t* res)
{
  if (ZCpuInfo::instance().bAVX512BW) {
    saturate_sub_avx512(x, y, count, res);
    return;
  } else if (ZCpuInfo::instance().bAVX2) {
    saturate_sub_avx2(x, y, count, res);
    return;
  }

  size_t i = 0;
  if (count >= 8) {
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
}

} // namespace nim
