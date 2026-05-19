#include "zgpusysteminfo.h"

#include "zwindowsheader.h"
#include <dxgi.h>
#include <memory>

namespace nim {

namespace {

template<class T>
struct ComRelease
{
  void operator()(T* ptr) const
  {
    if (ptr != nullptr) {
      ptr->Release();
    }
  }
};

template<class T>
using ComPtr = std::unique_ptr<T, ComRelease<T>>;

} // namespace

ZGpuMemoryCapacityInfo detectSystemGpuMemoryCapacity()
{
  ZGpuMemoryCapacityInfo info;

  IDXGIFactory1* factoryRaw = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factoryRaw)))) {
    return info;
  }
  ComPtr<IDXGIFactory1> factory(factoryRaw);

  for (UINT index = 0;; ++index) {
    IDXGIAdapter1* adapterRaw = nullptr;
    const HRESULT hr = factory->EnumAdapters1(index, &adapterRaw);
    if (hr == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    if (FAILED(hr)) {
      continue;
    }
    ComPtr<IDXGIAdapter1> adapter(adapterRaw);

    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) {
      continue;
    }
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
      continue;
    }
    if (desc.DedicatedVideoMemory <= info.capacityBytes) {
      continue;
    }

    info.capacityBytes = static_cast<uint64_t>(desc.DedicatedVideoMemory);
    info.hasUnifiedMemory = false;
    info.source = QStringLiteral("DXGI DedicatedVideoMemory");
    if (desc.Description[0] != L'\0') {
      info.source += QStringLiteral(" (");
      info.source += QString::fromWCharArray(desc.Description);
      info.source += QStringLiteral(")");
    }
  }

  return info;
}

} // namespace nim
