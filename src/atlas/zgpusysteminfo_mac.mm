#include "zgpusysteminfo.h"

#import <Metal/Metal.h>

namespace nim {

ZGpuMemoryCapacityInfo detectSystemGpuMemoryCapacity()
{
  ZGpuMemoryCapacityInfo info;

  @autoreleasepool {
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    for (id<MTLDevice> device in devices) {
      const uint64_t capacityBytes = [device recommendedMaxWorkingSetSize];
      if (capacityBytes <= info.capacityBytes) {
        continue;
      }

      const char* deviceName = [[device name] UTF8String];
      info.capacityBytes = capacityBytes;
      info.hasUnifiedMemory = [device hasUnifiedMemory];
      info.source = QStringLiteral("Metal recommendedMaxWorkingSetSize");
      if (deviceName != nullptr && deviceName[0] != '\0') {
        info.source += QStringLiteral(" (");
        info.source += QString::fromUtf8(deviceName);
        info.source += QStringLiteral(")");
      }
    }
  }

  return info;
}

} // namespace nim
