#include "zneutubeedt3d.h"

#include "zcancellation.h"
#include "zexception.h"
#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/CurrentExecutor.h>
#include <folly/coro/Task.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/executors/ThreadPoolExecutor.h>

DEFINE_bool(atlas_edt3d_parallel,
            true,
            "Parallelize the 3D distance transform (EDT) passes with folly's global CPU executor. "
            "Results are deterministic and identical; this mainly improves auto-trace seed extraction throughput.");

namespace nim {

namespace {

constexpr uint16_t kU16Inf = 0xFFFFu;
constexpr float kFloatInf = 1E20f;

struct DtScratch
{
  std::vector<uint16_t> f;
  std::vector<uint16_t> dd;
  std::vector<uint8_t> m;
  std::vector<int> v;
  std::vector<float> z;

  void ensureSize(size_t len)
  {
    if (f.size() >= len) {
      return;
    }
    f.resize(len);
    dd.resize(len);
    m.resize(len);
    v.resize(len);
    z.resize(len + 1);
  }
};

[[nodiscard]] size_t globalCpuExecutorThreadCountOrFallback()
{
  auto cpuExecutor = folly::getGlobalCPUExecutor();
  if (auto* threadPool = dynamic_cast<folly::ThreadPoolExecutor*>(cpuExecutor.get())) {
    const size_t threads = threadPool->numThreads();
    if (threads > 0) {
      return threads;
    }
  }

  const size_t fallback = static_cast<size_t>(std::thread::hardware_concurrency());
  return std::max<size_t>(1, fallback);
}

void dt1d_first_m_mu16(uint16_t* d, long n, const uint16_t* f, int* v, float* z)
{
  int q = 0;
  for (q = 1; q < n; ++q) {
    if (f[q - 1] != f[q]) {
      break;
    }
  }

  if (q == n) {
    return;
  }

  if (f[0] == 0) {
    v[0] = 0;
  } else {
    v[0] = q;
  }

  int k = 0;
  z[0] = -kFloatInf;
  z[1] = +kFloatInf;

  for (q = v[0] + 1; q < n; ++q) {
    if (f[q] == 0) {
      float s = static_cast<float>(q + v[k]) * 0.5f;
      while (s <= z[k]) {
        --k;
        s = static_cast<float>(q + v[k]) * 0.5f;
      }

      ++k;
      v[k] = q;
      z[k] = s;
      z[k + 1] = +kFloatInf;
    }
  }

  k = 0;
  for (q = 0; q < n; ++q) {
    if (d[q] > 0) {
      while (z[k + 1] < q) {
        ++k;
      }

      if (f[q] > 0) {
        d[q] = static_cast<uint16_t>(std::abs(q - v[k]));
      }
    }
  }
}

void dt1d_second_m_mu16(const uint16_t* f, long n, uint16_t* d, int* v, float* z, const uint8_t* m, int sqr_field)
{
  int q = 0;
  for (q = 0; q < n; ++q) {
    if (m[q] == 0) {
      break;
    }
  }

  if (q == n) {
    return;
  }

  v[0] = q;

  int k = 0;
  z[0] = -kFloatInf;
  z[1] = +kFloatInf;

  for (q = v[0] + 1; q < n; ++q) {
    if (m[q] == 0) {
      float s = static_cast<float>(f[q]) - static_cast<float>(f[v[k]]);
      if (sqr_field == 0) {
        s *= static_cast<float>(f[q]) + static_cast<float>(f[v[k]]);
      }
      s = (s / static_cast<float>(q - v[k]) + static_cast<float>(q + v[k])) / 2.0f;

      while (s <= z[k]) {
        --k;
        s = static_cast<float>(f[q]) - static_cast<float>(f[v[k]]);
        if (sqr_field == 0) {
          s *= static_cast<float>(f[q]) + static_cast<float>(f[v[k]]);
        }
        s = (s / static_cast<float>(q - v[k]) + static_cast<float>(q + v[k])) / 2.0f;
      }

      ++k;
      v[k] = q;
      z[k] = s;
      z[k + 1] = +kFloatInf;
    }
  }

  k = 0;
  for (q = 0; q <= n - 1; ++q) {
    if (f[q] > 0) {
      while (z[++k] < q) {}
      --k;

      const int dx = q - v[k];
      int64_t df = static_cast<int64_t>(dx) * static_cast<int64_t>(dx);
      if (sqr_field == 0) {
        const int64_t fv = static_cast<int64_t>(f[v[k]]);
        df += fv * fv;
      } else {
        df += static_cast<int64_t>(f[v[k]]);
      }

      if (df > static_cast<int64_t>(kU16Inf)) {
        d[q] = static_cast<uint16_t>(kU16Inf - 1);
      } else {
        d[q] = static_cast<uint16_t>(df);
      }
    } else {
      d[q] = 0;
    }
  }
}

folly::coro::Task<void> dt3dMu16XPassSliceTask(uint16_t* data,
                                               long w,
                                               long h,
                                               long plane,
                                               int pad,
                                               long kz,
                                               size_t lenS,
                                               const folly::CancellationToken& cancellationToken)
{
  // `co_withExecutor()` may begin execution on the awaiting thread and then reschedule on the desired executor.
  // Ensure thread-local scratch is initialized on the executor thread we actually run the compute on.
  co_await folly::coro::co_reschedule_on_current_executor;

  maybeCancel(cancellationToken);

  static thread_local DtScratch scratch;
  scratch.ensureSize(lenS);
  auto* f = scratch.f.data();
  auto* dd = scratch.dd.data();
  auto* v = scratch.v.data();
  auto* z = scratch.z.data();

  const long tmp_k = kz * plane;
  for (long jy = 0; jy < h; ++jy) {
    if (((jy & 63L) == 0L)) {
      maybeCancel(cancellationToken);
    }
    const long tmp_j = jy * w;
    if (pad == 1) {
      f[0] = 0;
      f[static_cast<size_t>(w) + 1] = 0;
      dd[0] = 0;
      dd[static_cast<size_t>(w) + 1] = 0;
    }

    std::copy_n(data + tmp_k + tmp_j, static_cast<size_t>(w), f + pad);
    std::copy_n(data + tmp_k + tmp_j, static_cast<size_t>(w), dd + pad);
    dt1d_first_m_mu16(dd, w + pad * 2L, f, v, z);
    std::copy_n(dd + pad, static_cast<size_t>(w), data + tmp_k + tmp_j);
  }

  co_return;
}

folly::coro::Task<void> dt3dMu16YPassSliceIxBlockTask(uint16_t* data,
                                                      long w,
                                                      long h,
                                                      long plane,
                                                      int pad,
                                                      long kz,
                                                      long ixStart,
                                                      long ixEnd,
                                                      size_t lenS,
                                                      const folly::CancellationToken& cancellationToken)
{
  co_await folly::coro::co_reschedule_on_current_executor;

  maybeCancel(cancellationToken);

  static thread_local DtScratch scratch;
  scratch.ensureSize(lenS);
  auto* f = scratch.f.data();
  auto* dd = scratch.dd.data();
  auto* m = scratch.m.data();
  auto* v = scratch.v.data();
  auto* z = scratch.z.data();

  const long tmp_k = kz * plane;
  for (long ix = ixStart; ix < ixEnd; ++ix) {
    if ((((ix - ixStart) & 15L) == 0L)) {
      maybeCancel(cancellationToken);
    }
    if (pad == 1) {
      f[0] = 0;
      f[static_cast<size_t>(h) + 1] = 0;
      m[0] = 0;
      m[static_cast<size_t>(h) + 1] = 0;
      dd[0] = 0;
      dd[static_cast<size_t>(h) + 1] = 0;
    }

    for (long jy = pad; jy < h + pad; ++jy) {
      const uint16_t val = *(data + tmp_k + (jy - pad) * w + ix);
      f[static_cast<size_t>(jy)] = val;
      m[static_cast<size_t>(jy)] = (val == kU16Inf);
    }

    dt1d_second_m_mu16(f, h + pad * 2L, dd, v, z, m, 0);

    for (long jy = pad; jy < h + pad; ++jy) {
      *(data + tmp_k + (jy - pad) * w + ix) = dd[static_cast<size_t>(jy)];
    }
  }

  co_return;
}

folly::coro::Task<void> dt3dMu16ZPassJyBlockTask(uint16_t* data,
                                                 long w,
                                                 long h,
                                                 long d,
                                                 long plane,
                                                 int pad,
                                                 long jyStart,
                                                 long jyEnd,
                                                 uint8_t zPassBoundaryMEnd,
                                                 size_t lenS,
                                                 const folly::CancellationToken& cancellationToken)
{
  co_await folly::coro::co_reschedule_on_current_executor;

  maybeCancel(cancellationToken);

  static thread_local DtScratch scratch;
  scratch.ensureSize(lenS);
  auto* f = scratch.f.data();
  auto* dd = scratch.dd.data();
  auto* m = scratch.m.data();
  auto* v = scratch.v.data();
  auto* z = scratch.z.data();

  for (long jy = jyStart; jy < jyEnd; ++jy) {
    maybeCancel(cancellationToken);
    const long tmp_j = jy * w;
    for (long ix = 0; ix < w; ++ix) {
      if (pad == 1) {
        f[0] = 0;
        f[static_cast<size_t>(d) + 1] = 0;
        dd[0] = 0;
        dd[static_cast<size_t>(d) + 1] = 0;
        // See zPassBoundaryMEnd above for why this is not forced to 0.
        m[0] = 0;
        m[static_cast<size_t>(d) + 1] = zPassBoundaryMEnd;
      }

      for (long kz = pad; kz < d + pad; ++kz) {
        const uint16_t val = *(data + (kz - pad) * plane + tmp_j + ix);
        f[static_cast<size_t>(kz)] = val;
        m[static_cast<size_t>(kz)] = (val == kU16Inf);
      }

      dt1d_second_m_mu16(f, d + pad * 2L, dd, v, z, m, 1);

      for (long kz = pad; kz < d + pad; ++kz) {
        *(data + (kz - pad) * plane + tmp_j + ix) = dd[static_cast<size_t>(kz)];
      }
    }
  }

  co_return;
}

void dt3d_mu16(uint16_t* data, const long* sz, int pad, const folly::CancellationToken& cancellationToken)
{
  maybeCancel(cancellationToken);

  const long w = sz[0];
  const long h = sz[1];
  const long d = sz[2];

  const long plane = h * w;

  long len = std::max({w, h, d});
  len += pad * 2;

  if (FLAGS_atlas_edt3d_parallel) {
    const size_t lenS = static_cast<size_t>(len);
    const size_t maxInFlight = globalCpuExecutorThreadCountOrFallback();
    auto cpuExecutor = folly::getGlobalCPUExecutor();

    // X pass: per-row distance to background.
    {
      std::vector<folly::coro::TaskWithExecutor<void>> tasks;
      tasks.reserve(static_cast<size_t>(d));
      for (long kz = 0; kz < d; ++kz) {
        tasks.push_back(
          folly::coro::co_withExecutor(cpuExecutor,
                                       dt3dMu16XPassSliceTask(data, w, h, plane, pad, kz, lenS, cancellationToken)));
      }
      folly::coro::blockingWait(folly::coro::collectAllWindowed(std::move(tasks), maxInFlight));
    }

    // Legacy quirk: the underlying C implementation does not reset `m[sz[2] + 1]` for the Z pass. When sz[1] >= sz[2],
    // it reuses whatever value was last written to that index during the Y pass (which corresponds to the final Y-pass
    // column at y=sz[2]). Compute that deterministic value from X-pass output so the parallel implementation matches
    // the legacy loop-order semantics.
    uint8_t zPassBoundaryMEnd = uint8_t{0};
    if (pad == 1 && w > 0 && h > 0 && d > 0 && h > d) {
      const long kz = d - 1;
      const long ix = w - 1;
      const long y = d; // j=sz[2]+1 in the Y pass corresponds to original y=sz[2] (0-based)
      const size_t offset = static_cast<size_t>(kz) * static_cast<size_t>(plane) +
                            static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(ix);
      zPassBoundaryMEnd = (data[offset] == kU16Inf) ? uint8_t{1} : uint8_t{0};
    }

    // Y pass: squared 2D distance field (per-slice).
    {
      constexpr long kIxBlock = 64;
      CHECK(kIxBlock > 0);

      std::vector<folly::coro::TaskWithExecutor<void>> tasks;
      const long blocksPerSlice = (w + kIxBlock - 1) / kIxBlock;
      tasks.reserve(static_cast<size_t>(d) * static_cast<size_t>(blocksPerSlice));

      for (long kz = 0; kz < d; ++kz) {
        for (long ixStart = 0; ixStart < w; ixStart += kIxBlock) {
          const long ixEnd = std::min(w, ixStart + kIxBlock);
          tasks.push_back(folly::coro::co_withExecutor(
            cpuExecutor,
            dt3dMu16YPassSliceIxBlockTask(data, w, h, plane, pad, kz, ixStart, ixEnd, lenS, cancellationToken)));
        }
      }

      folly::coro::blockingWait(folly::coro::collectAllWindowed(std::move(tasks), maxInFlight));
    }

    // Z pass: squared 3D distance field.
    {
      constexpr long kJyBlock = 16;
      CHECK(kJyBlock > 0);

      std::vector<folly::coro::TaskWithExecutor<void>> tasks;
      const long blocks = (h + kJyBlock - 1) / kJyBlock;
      tasks.reserve(static_cast<size_t>(blocks));

      for (long jyStart = 0; jyStart < h; jyStart += kJyBlock) {
        const long jyEnd = std::min(h, jyStart + kJyBlock);
        tasks.push_back(folly::coro::co_withExecutor(cpuExecutor,
                                                     dt3dMu16ZPassJyBlockTask(data,
                                                                              w,
                                                                              h,
                                                                              d,
                                                                              plane,
                                                                              pad,
                                                                              jyStart,
                                                                              jyEnd,
                                                                              zPassBoundaryMEnd,
                                                                              lenS,
                                                                              cancellationToken)));
      }

      folly::coro::blockingWait(folly::coro::collectAllWindowed(std::move(tasks), maxInFlight));
    }

    return;
  }

  std::vector<uint16_t> f(static_cast<size_t>(len));
  std::vector<uint16_t> dd(static_cast<size_t>(len));
  std::vector<uint8_t> m(static_cast<size_t>(len));
  std::vector<int> v(static_cast<size_t>(len));
  std::vector<float> z(static_cast<size_t>(len + 1));

  if (pad == 1) {
    f[0] = 0;
    f[static_cast<size_t>(w) + 1] = 0;
    dd[0] = 0;
    dd[static_cast<size_t>(w) + 1] = 0;
  }

  // X pass: per-row distance to background.
  for (long kz = 0; kz < d; ++kz) {
    maybeCancel(cancellationToken);
    const long tmp_k = kz * plane;
    for (long jy = 0; jy < h; ++jy) {
      if (((jy & 255L) == 0L)) {
        maybeCancel(cancellationToken);
      }
      const long tmp_j = jy * w;
      std::copy_n(data + tmp_k + tmp_j, static_cast<size_t>(w), f.begin() + pad);
      std::copy_n(data + tmp_k + tmp_j, static_cast<size_t>(w), dd.begin() + pad);
      dt1d_first_m_mu16(dd.data(), w + pad * 2L, f.data(), v.data(), z.data());
      std::copy_n(dd.begin() + pad, static_cast<size_t>(w), data + tmp_k + tmp_j);
    }
  }

  if (pad == 1) {
    f[0] = 0;
    f[static_cast<size_t>(h) + 1] = 0;
    m[0] = 0;
    m[static_cast<size_t>(h) + 1] = 0;
    dd[0] = 0;
    dd[static_cast<size_t>(h) + 1] = 0;
  }

  // Y pass: squared 2D distance field (per-slice).
  for (long kz = 0; kz < d; ++kz) {
    maybeCancel(cancellationToken);
    const long tmp_k = kz * plane;
    for (long ix = 0; ix < w; ++ix) {
      if (((ix & 255L) == 0L)) {
        maybeCancel(cancellationToken);
      }
      for (long jy = pad; jy < h + pad; ++jy) {
        const uint16_t val = *(data + tmp_k + (jy - pad) * w + ix);
        f[static_cast<size_t>(jy)] = val;
        m[static_cast<size_t>(jy)] = (val == kU16Inf);
      }

      dt1d_second_m_mu16(f.data(), h + pad * 2L, dd.data(), v.data(), z.data(), m.data(), 0);

      for (long jy = pad; jy < h + pad; ++jy) {
        *(data + tmp_k + (jy - pad) * w + ix) = dd[static_cast<size_t>(jy)];
      }
    }
  }

  if (pad == 1) {
    f[0] = 0;
    f[static_cast<size_t>(d) + 1] = 0;
    dd[0] = 0;
    dd[static_cast<size_t>(d) + 1] = 0;
  }

  // Z pass: squared 3D distance field.
  for (long jy = 0; jy < h; ++jy) {
    maybeCancel(cancellationToken);
    const long tmp_j = jy * w;
    for (long ix = 0; ix < w; ++ix) {
      if (((ix & 255L) == 0L)) {
        maybeCancel(cancellationToken);
      }
      for (long kz = pad; kz < d + pad; ++kz) {
        const uint16_t val = *(data + (kz - pad) * plane + tmp_j + ix);
        f[static_cast<size_t>(kz)] = val;
        m[static_cast<size_t>(kz)] = (val == kU16Inf);
      }

      dt1d_second_m_mu16(f.data(), d + pad * 2L, dd.data(), v.data(), z.data(), m.data(), 1);

      for (long kz = pad; kz < d + pad; ++kz) {
        *(data + (kz - pad) * plane + tmp_j + ix) = dd[static_cast<size_t>(kz)];
      }
    }
  }
}

void dt3d_binary_mu16(uint16_t* data, const long* sz, int pad, const folly::CancellationToken& cancellationToken)
{
  const size_t length = static_cast<size_t>(sz[0]) * static_cast<size_t>(sz[1]) * static_cast<size_t>(sz[2]);
  constexpr size_t kCancelCheckEvery = 1u << 20;
  size_t untilCheck = kCancelCheckEvery;
  for (size_t i = 0; i < length; ++i) {
    if (--untilCheck == 0) {
      maybeCancel(cancellationToken);
      untilCheck = kCancelCheckEvery;
    }
    data[i] = (data[i] > 0) ? kU16Inf : uint16_t{0};
  }

  dt3d_mu16(data, sz, pad, cancellationToken);
}

} // namespace

ZImg bwdistSquaredU16LegacyLike(const ZImg& binaryMask, int pad)
{
  return bwdistSquaredU16LegacyLike(binaryMask, pad, folly::CancellationToken{});
}

ZImg bwdistSquaredU16LegacyLike(const ZImg& binaryMask, int pad, const folly::CancellationToken& cancellationToken)
{
  if (binaryMask.isEmpty()) {
    return {};
  }

  CHECK(binaryMask.numChannels() == 1);
  CHECK(binaryMask.numTimes() == 1);
  CHECK(binaryMask.isType<uint8_t>()) << "Expected uint8 input, got " << binaryMask.info();
  maybeCancel(cancellationToken);

  ZImgInfo info = binaryMask.info();
  info.setVoxelFormat<uint16_t>();
  ZImg out(info);

  const size_t voxelNumber = binaryMask.voxelNumber();
  const auto* inData = binaryMask.timeData<uint8_t>(0);
  auto* outData = out.timeData<uint16_t>(0);

  constexpr size_t kCancelCheckEvery = 1u << 20;
  size_t untilCheck = kCancelCheckEvery;
  for (size_t i = 0; i < voxelNumber; ++i) {
    if (--untilCheck == 0) {
      maybeCancel(cancellationToken);
      untilCheck = kCancelCheckEvery;
    }
    outData[i] = static_cast<uint16_t>(inData[i]);
  }

  long sz[3];
  sz[0] = static_cast<long>(binaryMask.width());
  sz[1] = static_cast<long>(binaryMask.height());
  sz[2] = static_cast<long>(binaryMask.depth());

  // Legacy Stack_Bwdist_L_U16(..., pad) calls dt3d_binary_mu16(..., pad=!pad).
  const int dtPad = (pad == 0) ? 1 : 0;
  dt3d_binary_mu16(outData, sz, dtPad, cancellationToken);

  return out;
}

} // namespace nim
