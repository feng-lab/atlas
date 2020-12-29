#pragma once

#include <cstdint>
#include <cstddef>

namespace nim {

void saturate_add_avx512(const uint8_t* x, const uint8_t* y, size_t count, uint8_t* res);

void saturate_add_avx512(const uint8_t* x, uint8_t y, size_t count, uint8_t* res);

void saturate_add_avx512(const int8_t* x, const int8_t* y, size_t count, int8_t* res);

void saturate_add_avx512(const int8_t* x, int8_t y, size_t count, int8_t* res);

void saturate_add_avx512(const uint16_t* x, const uint16_t* y, size_t count, uint16_t* res);

void saturate_add_avx512(const uint16_t* x, uint16_t y, size_t count, uint16_t* res);

void saturate_add_avx512(const int16_t* x, const int16_t* y, size_t count, int16_t* res);

void saturate_add_avx512(const int16_t* x, int16_t y, size_t count, int16_t* res);

void saturate_sub_avx512(const uint8_t* x, const uint8_t* y, size_t count, uint8_t* res);

void saturate_sub_avx512(const uint8_t* x, uint8_t y, size_t count, uint8_t* res);

void saturate_sub_avx512(const int8_t* x, const int8_t* y, size_t count, int8_t* res);

void saturate_sub_avx512(const int8_t* x, int8_t y, size_t count, int8_t* res);

void saturate_sub_avx512(const uint16_t* x, const uint16_t* y, size_t count, uint16_t* res);

void saturate_sub_avx512(const uint16_t* x, uint16_t y, size_t count, uint16_t* res);

void saturate_sub_avx512(const int16_t* x, const int16_t* y, size_t count, int16_t* res);

void saturate_sub_avx512(const int16_t* x, int16_t y, size_t count, int16_t* res);

} // namespace nim
