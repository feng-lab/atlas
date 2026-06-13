#include "zsaturateoperation.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "zsaturateoperation.cpp"
#include "hwy/foreach_target.h"
#include "hwy/highway.h"

HWY_BEFORE_NAMESPACE();

namespace nim {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

namespace detail {

struct SaturatedAddOp
{
  template<typename V>
  HWY_INLINE V operator()(V x, V y) const
  {
    return hn::SaturatedAdd(x, y);
  }

  template<typename T>
  HWY_INLINE T scalar(T x, T y) const
  {
    return nim::saturate_add(x, y);
  }
};

struct SaturatedSubOp
{
  template<typename V>
  HWY_INLINE V operator()(V x, V y) const
  {
    return hn::SaturatedSub(x, y);
  }

  template<typename T>
  HWY_INLINE T scalar(T x, T y) const
  {
    return nim::saturate_sub(x, y);
  }
};

template<typename T, typename Op>
void applyArrayArray(const T* HWY_RESTRICT x, const T* HWY_RESTRICT y, size_t count, T* HWY_RESTRICT res)
{
  const hn::ScalableTag<T> d;
  const size_t lanes = hn::Lanes(d);
  const Op op;

  size_t i = 0;
  for (; i + lanes <= count; i += lanes) {
    hn::StoreU(op(hn::LoadU(d, x + i), hn::LoadU(d, y + i)), d, res + i);
  }

  for (; i < count; ++i) {
    res[i] = op.scalar(x[i], y[i]);
  }
}

template<typename T, typename Op>
void applyArrayScalar(const T* HWY_RESTRICT x, T y, size_t count, T* HWY_RESTRICT res)
{
  const hn::ScalableTag<T> d;
  const size_t lanes = hn::Lanes(d);
  const auto yv = hn::Set(d, y);
  const Op op;

  size_t i = 0;
  for (; i + lanes <= count; i += lanes) {
    hn::StoreU(op(hn::LoadU(d, x + i), yv), d, res + i);
  }

  for (; i < count; ++i) {
    res[i] = op.scalar(x[i], y);
  }
}

} // namespace detail

void saturateAddU8Array(const uint8_t* HWY_RESTRICT x,
                        const uint8_t* HWY_RESTRICT y,
                        size_t count,
                        uint8_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<uint8_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddU8Scalar(const uint8_t* HWY_RESTRICT x, uint8_t y, size_t count, uint8_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<uint8_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddI8Array(const int8_t* HWY_RESTRICT x,
                        const int8_t* HWY_RESTRICT y,
                        size_t count,
                        int8_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<int8_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddI8Scalar(const int8_t* HWY_RESTRICT x, int8_t y, size_t count, int8_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<int8_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddU16Array(const uint16_t* HWY_RESTRICT x,
                         const uint16_t* HWY_RESTRICT y,
                         size_t count,
                         uint16_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<uint16_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddU16Scalar(const uint16_t* HWY_RESTRICT x, uint16_t y, size_t count, uint16_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<uint16_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddI16Array(const int16_t* HWY_RESTRICT x,
                         const int16_t* HWY_RESTRICT y,
                         size_t count,
                         int16_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<int16_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddI16Scalar(const int16_t* HWY_RESTRICT x, int16_t y, size_t count, int16_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<int16_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddU32Array(const uint32_t* HWY_RESTRICT x,
                         const uint32_t* HWY_RESTRICT y,
                         size_t count,
                         uint32_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<uint32_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddU32Scalar(const uint32_t* HWY_RESTRICT x, uint32_t y, size_t count, uint32_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<uint32_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddI32Array(const int32_t* HWY_RESTRICT x,
                         const int32_t* HWY_RESTRICT y,
                         size_t count,
                         int32_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<int32_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddI32Scalar(const int32_t* HWY_RESTRICT x, int32_t y, size_t count, int32_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<int32_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddU64Array(const uint64_t* HWY_RESTRICT x,
                         const uint64_t* HWY_RESTRICT y,
                         size_t count,
                         uint64_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<uint64_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddU64Scalar(const uint64_t* HWY_RESTRICT x, uint64_t y, size_t count, uint64_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<uint64_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddI64Array(const int64_t* HWY_RESTRICT x,
                         const int64_t* HWY_RESTRICT y,
                         size_t count,
                         int64_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<int64_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateAddI64Scalar(const int64_t* HWY_RESTRICT x, int64_t y, size_t count, int64_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<int64_t, detail::SaturatedAddOp>(x, y, count, res);
}

void saturateSubU8Array(const uint8_t* HWY_RESTRICT x,
                        const uint8_t* HWY_RESTRICT y,
                        size_t count,
                        uint8_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<uint8_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubU8Scalar(const uint8_t* HWY_RESTRICT x, uint8_t y, size_t count, uint8_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<uint8_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubI8Array(const int8_t* HWY_RESTRICT x,
                        const int8_t* HWY_RESTRICT y,
                        size_t count,
                        int8_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<int8_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubI8Scalar(const int8_t* HWY_RESTRICT x, int8_t y, size_t count, int8_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<int8_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubU16Array(const uint16_t* HWY_RESTRICT x,
                         const uint16_t* HWY_RESTRICT y,
                         size_t count,
                         uint16_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<uint16_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubU16Scalar(const uint16_t* HWY_RESTRICT x, uint16_t y, size_t count, uint16_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<uint16_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubI16Array(const int16_t* HWY_RESTRICT x,
                         const int16_t* HWY_RESTRICT y,
                         size_t count,
                         int16_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<int16_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubI16Scalar(const int16_t* HWY_RESTRICT x, int16_t y, size_t count, int16_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<int16_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubU32Array(const uint32_t* HWY_RESTRICT x,
                         const uint32_t* HWY_RESTRICT y,
                         size_t count,
                         uint32_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<uint32_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubU32Scalar(const uint32_t* HWY_RESTRICT x, uint32_t y, size_t count, uint32_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<uint32_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubI32Array(const int32_t* HWY_RESTRICT x,
                         const int32_t* HWY_RESTRICT y,
                         size_t count,
                         int32_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<int32_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubI32Scalar(const int32_t* HWY_RESTRICT x, int32_t y, size_t count, int32_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<int32_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubU64Array(const uint64_t* HWY_RESTRICT x,
                         const uint64_t* HWY_RESTRICT y,
                         size_t count,
                         uint64_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<uint64_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubU64Scalar(const uint64_t* HWY_RESTRICT x, uint64_t y, size_t count, uint64_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<uint64_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubI64Array(const int64_t* HWY_RESTRICT x,
                         const int64_t* HWY_RESTRICT y,
                         size_t count,
                         int64_t* HWY_RESTRICT res)
{
  detail::applyArrayArray<int64_t, detail::SaturatedSubOp>(x, y, count, res);
}

void saturateSubI64Scalar(const int64_t* HWY_RESTRICT x, int64_t y, size_t count, int64_t* HWY_RESTRICT res)
{
  detail::applyArrayScalar<int64_t, detail::SaturatedSubOp>(x, y, count, res);
}

} // namespace HWY_NAMESPACE
} // namespace nim

HWY_AFTER_NAMESPACE();

#if HWY_ONCE

namespace nim {

HWY_EXPORT(saturateAddU8Array);
HWY_EXPORT(saturateAddU8Scalar);
HWY_EXPORT(saturateAddI8Array);
HWY_EXPORT(saturateAddI8Scalar);
HWY_EXPORT(saturateAddU16Array);
HWY_EXPORT(saturateAddU16Scalar);
HWY_EXPORT(saturateAddI16Array);
HWY_EXPORT(saturateAddI16Scalar);
HWY_EXPORT(saturateAddU32Array);
HWY_EXPORT(saturateAddU32Scalar);
HWY_EXPORT(saturateAddI32Array);
HWY_EXPORT(saturateAddI32Scalar);
HWY_EXPORT(saturateAddU64Array);
HWY_EXPORT(saturateAddU64Scalar);
HWY_EXPORT(saturateAddI64Array);
HWY_EXPORT(saturateAddI64Scalar);
HWY_EXPORT(saturateSubU8Array);
HWY_EXPORT(saturateSubU8Scalar);
HWY_EXPORT(saturateSubI8Array);
HWY_EXPORT(saturateSubI8Scalar);
HWY_EXPORT(saturateSubU16Array);
HWY_EXPORT(saturateSubU16Scalar);
HWY_EXPORT(saturateSubI16Array);
HWY_EXPORT(saturateSubI16Scalar);
HWY_EXPORT(saturateSubU32Array);
HWY_EXPORT(saturateSubU32Scalar);
HWY_EXPORT(saturateSubI32Array);
HWY_EXPORT(saturateSubI32Scalar);
HWY_EXPORT(saturateSubU64Array);
HWY_EXPORT(saturateSubU64Scalar);
HWY_EXPORT(saturateSubI64Array);
HWY_EXPORT(saturateSubI64Scalar);

template<>
void saturate_add<uint8_t, const uint8_t>(const uint8_t* x, const uint8_t* y, size_t count, uint8_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddU8Array)(x, y, count, res);
}

template<>
void saturate_add<uint8_t, uint8_t>(const uint8_t* x, uint8_t y, size_t count, uint8_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddU8Scalar)(x, y, count, res);
}

template<>
void saturate_add<int8_t, const int8_t>(const int8_t* x, const int8_t* y, size_t count, int8_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddI8Array)(x, y, count, res);
}

template<>
void saturate_add<int8_t, int8_t>(const int8_t* x, int8_t y, size_t count, int8_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddI8Scalar)(x, y, count, res);
}

template<>
void saturate_add<uint16_t, const uint16_t>(const uint16_t* x, const uint16_t* y, size_t count, uint16_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddU16Array)(x, y, count, res);
}

template<>
void saturate_add<uint16_t, uint16_t>(const uint16_t* x, uint16_t y, size_t count, uint16_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddU16Scalar)(x, y, count, res);
}

template<>
void saturate_add<int16_t, const int16_t>(const int16_t* x, const int16_t* y, size_t count, int16_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddI16Array)(x, y, count, res);
}

template<>
void saturate_add<int16_t, int16_t>(const int16_t* x, int16_t y, size_t count, int16_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddI16Scalar)(x, y, count, res);
}

template<>
void saturate_add<uint32_t, const uint32_t>(const uint32_t* x, const uint32_t* y, size_t count, uint32_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddU32Array)(x, y, count, res);
}

template<>
void saturate_add<uint32_t, uint32_t>(const uint32_t* x, uint32_t y, size_t count, uint32_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddU32Scalar)(x, y, count, res);
}

template<>
void saturate_add<int32_t, const int32_t>(const int32_t* x, const int32_t* y, size_t count, int32_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddI32Array)(x, y, count, res);
}

template<>
void saturate_add<int32_t, int32_t>(const int32_t* x, int32_t y, size_t count, int32_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddI32Scalar)(x, y, count, res);
}

template<>
void saturate_add<uint64_t, const uint64_t>(const uint64_t* x, const uint64_t* y, size_t count, uint64_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddU64Array)(x, y, count, res);
}

template<>
void saturate_add<uint64_t, uint64_t>(const uint64_t* x, uint64_t y, size_t count, uint64_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddU64Scalar)(x, y, count, res);
}

template<>
void saturate_add<int64_t, const int64_t>(const int64_t* x, const int64_t* y, size_t count, int64_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddI64Array)(x, y, count, res);
}

template<>
void saturate_add<int64_t, int64_t>(const int64_t* x, int64_t y, size_t count, int64_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateAddI64Scalar)(x, y, count, res);
}

template<>
void saturate_sub<uint8_t, const uint8_t>(const uint8_t* x, const uint8_t* y, size_t count, uint8_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubU8Array)(x, y, count, res);
}

template<>
void saturate_sub<uint8_t, uint8_t>(const uint8_t* x, uint8_t y, size_t count, uint8_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubU8Scalar)(x, y, count, res);
}

template<>
void saturate_sub<int8_t, const int8_t>(const int8_t* x, const int8_t* y, size_t count, int8_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubI8Array)(x, y, count, res);
}

template<>
void saturate_sub<int8_t, int8_t>(const int8_t* x, int8_t y, size_t count, int8_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubI8Scalar)(x, y, count, res);
}

template<>
void saturate_sub<uint16_t, const uint16_t>(const uint16_t* x, const uint16_t* y, size_t count, uint16_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubU16Array)(x, y, count, res);
}

template<>
void saturate_sub<uint16_t, uint16_t>(const uint16_t* x, uint16_t y, size_t count, uint16_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubU16Scalar)(x, y, count, res);
}

template<>
void saturate_sub<int16_t, const int16_t>(const int16_t* x, const int16_t* y, size_t count, int16_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubI16Array)(x, y, count, res);
}

template<>
void saturate_sub<int16_t, int16_t>(const int16_t* x, int16_t y, size_t count, int16_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubI16Scalar)(x, y, count, res);
}

template<>
void saturate_sub<uint32_t, const uint32_t>(const uint32_t* x, const uint32_t* y, size_t count, uint32_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubU32Array)(x, y, count, res);
}

template<>
void saturate_sub<uint32_t, uint32_t>(const uint32_t* x, uint32_t y, size_t count, uint32_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubU32Scalar)(x, y, count, res);
}

template<>
void saturate_sub<int32_t, const int32_t>(const int32_t* x, const int32_t* y, size_t count, int32_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubI32Array)(x, y, count, res);
}

template<>
void saturate_sub<int32_t, int32_t>(const int32_t* x, int32_t y, size_t count, int32_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubI32Scalar)(x, y, count, res);
}

template<>
void saturate_sub<uint64_t, const uint64_t>(const uint64_t* x, const uint64_t* y, size_t count, uint64_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubU64Array)(x, y, count, res);
}

template<>
void saturate_sub<uint64_t, uint64_t>(const uint64_t* x, uint64_t y, size_t count, uint64_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubU64Scalar)(x, y, count, res);
}

template<>
void saturate_sub<int64_t, const int64_t>(const int64_t* x, const int64_t* y, size_t count, int64_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubI64Array)(x, y, count, res);
}

template<>
void saturate_sub<int64_t, int64_t>(const int64_t* x, int64_t y, size_t count, int64_t* res)
{
  HWY_DYNAMIC_DISPATCH(saturateSubI64Scalar)(x, y, count, res);
}

} // namespace nim

#endif // HWY_ONCE
