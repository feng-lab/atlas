#include "zcpuinfo.h"

#include "zbitset.h"
#include "zlog.h"
#include <cpuinfo.h>
#include <QProcess>
#include <array>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include "zwindowsheader.h"
#include <intrin.h>
#elif defined(__APPLE__)

#include <sys/sysctl.h>
#include <cpuid.h>

#else
#include <sys/sysinfo.h>
#include <cpuid.h>
#include <unistd.h>
#endif

// http://stackoverflow.com/questions/6121792/how-to-check-if-a-cpu-supports-the-sse3-instruction-set @Mysticial
// https://msdn.microsoft.com/en-us/library/hskdteyh(v=vs.140).aspx

namespace {

#ifdef _WIN32

#define cpuid __cpuid
#define cpuidex __cpuidex

using LPFN_GLPI = BOOL(WINAPI*)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);

// Helper function to count set bits in the processor mask.
DWORD CountSetBits(ULONG_PTR bitMask)
{
  DWORD LSHIFT = sizeof(ULONG_PTR) * 8 - 1;
  DWORD bitSetCount = 0;
  ULONG_PTR bitTest = (ULONG_PTR)1 << LSHIFT;
  DWORD i;

  for (i = 0; i <= LSHIFT; ++i) {
    bitSetCount += ((bitMask & bitTest) ? 1 : 0);
    bitTest /= 2;
  }

  return bitSetCount;
}

#else

inline void cpuid(int32_t cpu_info[4], int32_t info_type)
{
  __cpuid(info_type, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
}

inline void cpuidex(int32_t cpu_info[4], int32_t info_type, int32_t info_index)
{
  __cpuid_count(info_type, info_index, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
}

inline uint64_t _xgetbv(uint32_t index)
{
  uint32_t eax, edx;
  __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
  return (static_cast<uint64_t>(edx) << 32) | eax;
}

#endif

} // namespace

namespace nim {

ZCpuInfo& ZCpuInfo::instance()
{
  static ZCpuInfo cpuInfo;
  return cpuInfo;
}

ZCpuInfo::ZCpuInfo()
{
  cpuinfo_initialize();
  detectCpuInfo();
}

void ZCpuInfo::logCpuInfo() const
{
  for (uint32_t i = 0; i < cpuinfo_get_packages_count(); ++i) {
    auto package = cpuinfo_get_package(i);
    LOG(INFO) << fmt::format("Package {} (name:{}), cluster {}-{}, core {}-{}, processor {}-{}",
                             i,
                             package->name,
                             package->cluster_start,
                             package->cluster_start + package->cluster_count - 1,
                             package->core_start,
                             package->core_start + package->core_count - 1,
                             package->processor_start,
                             package->processor_start + package->processor_count - 1);
    for (uint32_t ci = package->cluster_start; ci < package->cluster_start + package->cluster_count; ++ci) {
      auto cluster = cpuinfo_get_cluster(ci);
      LOG(INFO) << fmt::format("    Cluster {} (id:{}), core {}-{}, processor {}-{}, frequency {}",
                               ci,
                               cluster->cluster_id,
                               cluster->core_start,
                               cluster->core_start + cluster->core_count - 1,
                               cluster->processor_start,
                               cluster->processor_start + cluster->processor_count - 1,
                               cluster->frequency);
      for (uint32_t cci = cluster->core_start; cci < cluster->core_start + cluster->core_count; ++cci) {
        auto core = cpuinfo_get_core(cci);
        LOG(INFO) << fmt::format("        Core {} (id:{}), processor {}-{}, frequency {}",
                                 cci,
                                 core->core_id,
                                 core->processor_start,
                                 core->processor_start + core->processor_count - 1,
                                 core->frequency);
        for (uint32_t pi = core->processor_start; pi < core->processor_start + core->processor_count; ++pi) {
          auto processor = cpuinfo_get_processor(pi);
          LOG(INFO) << fmt::format("            Processor {} (smt id:{}), cache l1i:{} l1d:{} l2:{} l3:{} l4:{}",
                                   pi,
                                   processor->smt_id,
                                   processor->cache.l1i ? processor->cache.l1i->size : 0,
                                   processor->cache.l1d ? processor->cache.l1d->size : 0,
                                   processor->cache.l2 ? processor->cache.l2->size : 0,
                                   processor->cache.l3 ? processor->cache.l3->size : 0,
                                   processor->cache.l4 ? processor->cache.l4->size : 0);
        }
      }
    }
  }
  for (uint32_t i = 0; i < cpuinfo_get_l1d_caches_count(); ++i) {
    auto cache = cpuinfo_get_l1d_cache(i);
    LOG(INFO) << fmt::format(
      "L1d cache {}, size:{}, associativity:{}, sets:{}, partitions:{}, line size:{}, processor {}-{}",
      i,
      cache->size,
      cache->associativity,
      cache->sets,
      cache->partitions,
      cache->line_size,
      cache->processor_start,
      cache->processor_start + cache->processor_count - 1);
  }
  for (uint32_t i = 0; i < cpuinfo_get_l1i_caches_count(); ++i) {
    auto cache = cpuinfo_get_l1i_cache(i);
    LOG(INFO) << fmt::format(
      "L1i cache {}, size:{}, associativity:{}, sets:{}, partitions:{}, line size:{}, processor {}-{}",
      i,
      cache->size,
      cache->associativity,
      cache->sets,
      cache->partitions,
      cache->line_size,
      cache->processor_start,
      cache->processor_start + cache->processor_count - 1);
  }
  for (uint32_t i = 0; i < cpuinfo_get_l2_caches_count(); ++i) {
    auto cache = cpuinfo_get_l2_cache(i);
    LOG(INFO) << fmt::format(
      "L2 cache {}, size:{}, associativity:{}, sets:{}, partitions:{}, line size:{}, processor {}-{}",
      i,
      cache->size,
      cache->associativity,
      cache->sets,
      cache->partitions,
      cache->line_size,
      cache->processor_start,
      cache->processor_start + cache->processor_count - 1);
  }
  for (uint32_t i = 0; i < cpuinfo_get_l3_caches_count(); ++i) {
    auto cache = cpuinfo_get_l3_cache(i);
    LOG(INFO) << fmt::format(
      "L3 cache {}, size:{}, associativity:{}, sets:{}, partitions:{}, line size:{}, processor {}-{}",
      i,
      cache->size,
      cache->associativity,
      cache->sets,
      cache->partitions,
      cache->line_size,
      cache->processor_start,
      cache->processor_start + cache->processor_count - 1);
  }
  for (uint32_t i = 0; i < cpuinfo_get_l4_caches_count(); ++i) {
    auto cache = cpuinfo_get_l4_cache(i);
    LOG(INFO) << fmt::format(
      "L4 cache {}, size:{}, associativity:{}, sets:{}, partitions:{}, line size:{}, processor {}-{}",
      i,
      cache->size,
      cache->associativity,
      cache->sets,
      cache->partitions,
      cache->line_size,
      cache->processor_start,
      cache->processor_start + cache->processor_count - 1);
  }

  LOG(INFO) << "CPU Vendor: " << vendor;
  LOG(INFO) << "CPU Brand: " << brand;
  LOG(INFO) << "Stepping ID: " << nSteppingID << " Model ID: " << nModel << " Family ID: " << nFamily
            << " Type: " << nProcessorType << " Ext.Model ID: " << nExtendedmodel
            << " Ext.Family ID: " << nExtendedfamily;
  LOG(INFO) << "Number of Cores: " << nPhysicalCores;
  LOG(INFO) << "Number of Threads: " << nLogicalCores;
  LOG(INFO) << "std thread hardware_concurrency: " << nStdHardwareConcurrency;
  LOG(INFO) << "Cache Line: " << nCacheLine;
  LOG(INFO) << "L1ICache: " << nL1ICacheSize;
  LOG(INFO) << "L1DCache: " << nL1DCacheSize;
  LOG(INFO) << "L2Cache: " << nL2CacheSize;
  LOG(INFO) << "L3Cache: " << nL3CacheSize;
  LOG(INFO) << "RAM: " << nPhysicalRAM;

  QString instructions =
    QString(bXOP ? "XOP " : "") + QString(bFMA ? "FMA " : "") + QString(bFMA4 ? "FMA4 " : "") +
    QString(b3DNow ? "3Dnow! " : "") + QString(b3DNowExt ? "3Dnow ext " : "") + QString(bMMX ? "MMX " : "") +
    QString(bMMXExtensions ? "MMX ext " : "") + QString(bSSE ? "SSE " : "") + QString(bSSE2 ? "SSE2 " : "") +
    QString(bSSE3 ? "SSE3 " : "") + QString(bSSSE3 ? "SSSE3 " : "") + QString(bSSE41 ? "SSE41 " : "") +
    QString(bSSE42 ? "SSE42 " : "") + QString(bSSE4A ? "SSE4A " : "") + QString(bAVX ? "AVX " : "") +
    QString(bAVX2 ? "AVX2 " : "") + QString(bBMI ? "BMI " : "") + QString(bAVX512F ? "AVX512F " : "") +
    QString(bAVX512DQ ? "AVX512DQ " : "") + QString(bAVX512PF ? "AVX512PF " : "") +
    QString(bAVX512ER ? "AVX512ER " : "") + QString(bAVX512CD ? "AVX512CD " : "") +
    QString(bAVX512BW ? "AVX512BW " : "") + QString(bAVX512VL ? "AVX512VL " : "");

  LOG(INFO) << instructions;

  instructions = QString(bCMPXCHG8B ? "CMPXCHG8B " : "") + QString(bCMPXCHG16B ? "CMPXCHG16B " : "") +
                 QString(bPOPCNT ? "POPCNT " : "") + QString(bLZCNT ? "LZCNT " : "") +
                 QString(bRDTSCP ? "RDTSCP " : "");

  LOG(INFO) << instructions;
}

void ZCpuInfo::detectCpuInfo()
{
  std::array<int, 4> cpui{
    {0, 0, 0, 0}
  };
  uint32_t nIds = 0;
  uint32_t nExIds = 0;
  bool isIntel = false;
  bool isAMD = false;
  std::bitset<32> f_1_EAX = 0;
  std::bitset<32> f_1_EBX = 0;
  std::bitset<32> f_1_ECX = 0;
  std::bitset<32> f_1_EDX = 0;
  std::bitset<32> f_7_EBX = 0;
  std::bitset<32> f_7_ECX = 0;
  std::bitset<32> f_81_ECX = 0;
  std::bitset<32> f_81_EDX = 0;
  std::vector<std::array<int, 4>> data;
  std::vector<std::array<int, 4>> extdata;

  // Calling __cpuid with 0x0 as the function_id argument
  // gets the number of the highest valid function ID.
  cpuid(cpui.data(), 0);
  nIds = cpui[0];

  for (uint32_t i = 0; i <= nIds; ++i) {
    cpuidex(cpui.data(), i, 0);
    data.push_back(cpui);
  }

  // Capture vendor string
  char vendorStr[0x20];
  std::memset(vendorStr, 0, sizeof(vendorStr));
  std::memcpy(vendorStr, &data[0][1], sizeof(int32_t));
  std::memcpy(vendorStr + 4, &data[0][3], sizeof(int32_t));
  std::memcpy(vendorStr + 8, &data[0][2], sizeof(int32_t));
  vendor = QString(vendorStr);
  if (vendor == "GenuineIntel") {
    isIntel = true;
  } else if (vendor == "AuthenticAMD") {
    isAMD = true;
  }

  // load bitset with flags for function 0x00000001
  if (nIds >= 1) {
    f_1_EAX = static_cast<uint64_t>(data[1][0]);
    f_1_EBX = static_cast<uint64_t>(data[1][1]);
    f_1_ECX = static_cast<uint64_t>(data[1][2]);
    f_1_EDX = static_cast<uint64_t>(data[1][3]);
  }

  // load bitset with flags for function 0x00000007
  if (nIds >= 7) {
    f_7_EBX = static_cast<uint64_t>(data[7][1]);
    f_7_ECX = static_cast<uint64_t>(data[7][2]);
  }

  // Calling __cpuid with 0x80000000 as the function_id argument
  // gets the number of the highest valid extended ID.
  cpuid(cpui.data(), static_cast<int32_t>(0x80000000));
  nExIds = static_cast<uint32_t>(cpui[0]);

  for (uint32_t i = 0x80000000; i <= nExIds; ++i) {
    cpuidex(cpui.data(), static_cast<int32_t>(i), 0);
    extdata.push_back(cpui);
  }

  // load bitset with flags for function 0x80000001
  if (nExIds >= 0x80000001) {
    f_81_ECX = extdata[1][2];
    f_81_EDX = extdata[1][3];
  }

  // Interpret CPU brand string if reported
  char brandStr[0x40];
  std::memset(brandStr, 0, sizeof(brandStr));
  if (nExIds >= 0x80000004) {
    std::memcpy(brandStr, extdata[2].data(), 16);
    std::memcpy(brandStr + 16, extdata[3].data(), 16);
    std::memcpy(brandStr + 32, extdata[4].data(), 16);
    brand = QString(brandStr);
  }

  // Interpret CPU feature information.
  bSSE3 = f_1_ECX[0];
  bPCLMULQDQ = f_1_ECX[1];
  bDTES64 = f_1_ECX[2];
  bMONITOR = f_1_ECX[3];
  bDSCPL = f_1_ECX[4];
  bVMX = f_1_ECX[5];
  bSMX = f_1_ECX[6];
  bEIST = f_1_ECX[7];
  bTM2 = f_1_ECX[8];
  bSSSE3 = f_1_ECX[9];
  bCNXTID = f_1_ECX[10];
  bSDBG = f_1_ECX[11];
  bFMA = f_1_ECX[12];
  bCMPXCHG16B = f_1_ECX[13];
  bxTPRUpdateControl = f_1_ECX[14];
  bPDCM = f_1_ECX[15];
  bPCID = f_1_ECX[17];
  bDCA = f_1_ECX[18];
  bSSE41 = f_1_ECX[19];
  bSSE42 = f_1_ECX[20];
  bx2APIC = f_1_ECX[21];
  bMOVBE = f_1_ECX[22];
  bPOPCNT = f_1_ECX[23];
  bTSCDeadline = f_1_ECX[24];
  bAESNI = f_1_ECX[25];
  bXSAVE = f_1_ECX[26];
  bOSXSAVE = f_1_ECX[27];
  bAVX = f_1_ECX[28];
  bF16C = f_1_ECX[29];
  bRDRAND = f_1_ECX[30];

  bFPU = f_1_EDX[0];
  bVME = f_1_EDX[1];
  bDE = f_1_EDX[2];
  bPSE = f_1_EDX[3];
  bTSC = f_1_EDX[4];
  bMSR = f_1_EDX[5];
  bPAE = f_1_EDX[6];
  bMCE = f_1_EDX[7];
  bCMPXCHG8B = f_1_EDX[8];
  bAPIC = f_1_EDX[9];
  bSEP = f_1_EDX[11];
  bMTRR = f_1_EDX[12];
  bPGE = f_1_EDX[13];
  bMCA = f_1_EDX[14];
  bCMOV = f_1_EDX[15];
  bPAT = f_1_EDX[16];
  bPSE36 = f_1_EDX[17];
  bPSN = f_1_EDX[18];
  bCLFSH = f_1_EDX[19];
  bDS = f_1_EDX[21];
  bACPI = f_1_EDX[22];
  bMMX = f_1_EDX[23];
  bFXSR = f_1_EDX[24];
  bSSE = f_1_EDX[25];
  bSSE2 = f_1_EDX[26];
  bSS = f_1_EDX[27];
  bHTT = f_1_EDX[28];
  bTM = f_1_EDX[29];
  bPBE = f_1_EDX[31];

  nSteppingID = bitsetRangeToValue(f_1_EAX, 0, 4);
  nModel = bitsetRangeToValue(f_1_EAX, 4, 8);
  nFamily = bitsetRangeToValue(f_1_EAX, 8, 12);
  nProcessorType = bitsetRangeToValue(f_1_EAX, 12, 14);
  nExtendedmodel = bitsetRangeToValue(f_1_EAX, 16, 20);
  nExtendedfamily = bitsetRangeToValue(f_1_EAX, 20, 28);

  nMaxLogicalProcessors = bHTT ? bitsetRangeToValue(f_1_EBX, 16, 24) : 1;
  nAPICPhysicalID = bitsetRangeToValue(f_1_EBX, 24, 32);

  bAVX2 = f_7_EBX[5];
  bBMI = f_7_EBX[3] && f_7_EBX[8];
  bAVX512F = f_7_EBX[16];
  bAVX512DQ = f_7_EBX[17];
  bRDSEED = f_7_EBX[18];
  bADX = f_7_EBX[19];
  bAVX512PF = f_7_EBX[26];
  bAVX512ER = f_7_EBX[27];
  bAVX512CD = f_7_EBX[28];
  bSHA = f_7_EBX[29];
  bAVX512BW = f_7_EBX[30];
  bAVX512VL = f_7_EBX[31];

  bPREFTEHCHWT1 = f_7_ECX[0];

  bool hasXYMM = false;
  bool hasZMM = false;
  if (bOSXSAVE) {
    uint64_t xcrFeatureMask = _xgetbv(0);
    std::bitset<64> xcr0(xcrFeatureMask);
    hasXYMM = xcr0[1] && xcr0[2];
    hasZMM = xcr0[5] && xcr0[6] && xcr0[7];
  }
  bAVX = bAVX && hasXYMM;

  bAESNI = bAESNI && bAVX;
  bPCLMULQDQ = bPCLMULQDQ && bAVX;
  bF16C = bF16C && bAVX;
  bFMA = bFMA && bAVX;
  bAVX2 = bAVX2 && bAVX;

  bAVX512F = bAVX512F && hasXYMM && hasZMM;
  bAVX512DQ = bAVX512DQ && bAVX512F;
  bAVX512PF = bAVX512PF && bAVX512F;
  bAVX512ER = bAVX512ER && bAVX512F;
  bAVX512CD = bAVX512CD && bAVX512F;
  bAVX512BW = bAVX512BW && bAVX512F;
  bAVX512VL = bAVX512VL && bAVX512F;

  bLZCNT = f_81_ECX[5] && isIntel;
  bABM = f_81_ECX[5] && isAMD;
  bSSE4A = f_81_ECX[6] && isAMD;
  bPREFTEHCHW = f_81_ECX[8];
  bXOP = f_81_ECX[11] && isAMD;
  bFMA4 = f_81_ECX[16] && isAMD;

  bMMXExtensions = f_81_EDX[22] && isAMD;
  bRDTSCP = f_81_EDX[27] && isIntel;
  b3DNowExt = f_81_EDX[30] && isAMD;
  b3DNow = f_81_EDX[31] && isAMD;

  detectCoreAndThreadNumber();
}

void ZCpuInfo::detectCoreAndThreadNumber()
{
  nPhysicalCores = cpuinfo_get_cores_count();
  nLogicalCores = cpuinfo_get_processors_count();

#ifdef _WIN32
  MEMORYSTATUSEX memInfo;
  memInfo.dwLength = sizeof(MEMORYSTATUSEX);
  GlobalMemoryStatusEx(&memInfo);
  DWORDLONG totalPhysMem = memInfo.ullTotalPhys;
  nPhysicalRAM = totalPhysMem;

  LPFN_GLPI glpi;
  BOOL done = FALSE;
  PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer = nullptr;
  PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr = nullptr;
  DWORD returnLength = 0;
  DWORD logicalProcessorCount = 0;
  DWORD numaNodeCount = 0;
  DWORD processorCoreCount = 0;
  DWORD processorL1CacheCount = 0;
  DWORD processorL2CacheCount = 0;
  DWORD processorL3CacheCount = 0;
  DWORD processorPackageCount = 0;
  DWORD byteOffset = 0;
  PCACHE_DESCRIPTOR Cache;

  glpi = (LPFN_GLPI)GetProcAddress(GetModuleHandle(TEXT("kernel32")), "GetLogicalProcessorInformation");
  if (!glpi) {
    LOG(ERROR) << "GetLogicalProcessorInformation is not supported.";
    return;
  }

  while (!done) {
    DWORD rc = glpi(buffer, &returnLength);

    if (FALSE == rc) {
      if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        if (buffer) {
          free(buffer);
        }

        buffer = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(returnLength);

        if (!buffer) {
          LOG(ERROR) << "Allocation PSYSTEM_LOGICAL_PROCESSOR_INFORMATION failure";
          return;
        }
      } else {
        LOG(ERROR) << "Error " << GetLastError();
        return;
      }
    } else {
      done = TRUE;
    }
  }

  ptr = buffer;

  while (byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnLength) {
    switch (ptr->Relationship) {
      case RelationNumaNode:
        // Non-NUMA systems report a single record of this type.
        numaNodeCount++;
        break;

      case RelationProcessorCore:
        processorCoreCount++;

        // A hyperthreaded core supplies more than one logical processor.
        logicalProcessorCount += CountSetBits(ptr->ProcessorMask);
        break;

      case RelationCache:
        // Cache data is in ptr->Cache, one CACHE_DESCRIPTOR structure for each cache.
        Cache = &ptr->Cache;
        nCacheLine = Cache->LineSize;
        if (Cache->Level == 1) {
          processorL1CacheCount++;
          if (Cache->Type == CacheInstruction) {
            nL1ICacheSize = std::max(nL1ICacheSize, static_cast<uint64_t>(Cache->Size));
          } else if (Cache->Type == CacheData) {
            nL1DCacheSize = std::max(nL1DCacheSize, static_cast<uint64_t>(Cache->Size));
          }
        } else if (Cache->Level == 2) {
          processorL2CacheCount++;
          if (Cache->Type == CacheUnified) {
            nL2CacheSize = std::max(nL2CacheSize, static_cast<uint64_t>(Cache->Size));
          }
        } else if (Cache->Level == 3) {
          processorL3CacheCount++;
          if (Cache->Type == CacheUnified) {
            nL3CacheSize = std::max(nL3CacheSize, static_cast<uint64_t>(Cache->Size));
          }
        }
        break;

      case RelationProcessorPackage:
        // Logical processors share a physical package.
        processorPackageCount++;
        break;

      default:
        LOG(ERROR) << "Unsupported LOGICAL_PROCESSOR_RELATIONSHIP value.";
        break;
    }
    byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    ++ptr;
  }

  LOG(INFO) << "Number of NUMA nodes: " << numaNodeCount;
  LOG(INFO) << "Number of physical processor packages: " << processorPackageCount;
  nPhysicalCores = processorCoreCount;
  nLogicalCores = logicalProcessorCount;

  free(buffer);
#elif defined(__APPLE__)
  int32_t tmpNumCores;
  size_t len = sizeof(tmpNumCores);
  if (sysctlbyname("hw.physicalcpu_max", &tmpNumCores, &len, nullptr, 0) == 0) {
    nPhysicalCores = tmpNumCores;
  }
  if (sysctlbyname("hw.logicalcpu_max", &tmpNumCores, &len, nullptr, 0) == 0) {
    nLogicalCores = tmpNumCores;
  }

  int nm[2];
  len = sizeof(nPhysicalRAM);
  nm[0] = CTL_HW;
  nm[1] = HW_MEMSIZE;
  if (sysctl(nm, 2, &nPhysicalRAM, &len, nullptr, 0) != 0) {
    nPhysicalRAM = 0;
  }

  uint32_t count = 0;
  len = sizeof(count);
  nm[0] = CTL_HW;
  nm[1] = HW_CACHELINE;
  if (sysctl(nm, 2, &count, &len, nullptr, 0) == 0) {
    nCacheLine = count;
  }

  count = 0;
  nm[0] = CTL_HW;
  nm[1] = HW_L1ICACHESIZE;
  if (sysctl(nm, 2, &count, &len, nullptr, 0) == 0) {
    nL1ICacheSize = count;
  }

  count = 0;
  nm[0] = CTL_HW;
  nm[1] = HW_L1DCACHESIZE;
  if (sysctl(nm, 2, &count, &len, nullptr, 0) == 0) {
    nL1DCacheSize = count;
  }

  count = 0;
  nm[0] = CTL_HW;
  nm[1] = HW_L2CACHESIZE;
  if (sysctl(nm, 2, &count, &len, nullptr, 0) == 0) {
    nL2CacheSize = count;
  }

  count = 0;
  nm[0] = CTL_HW;
  nm[1] = HW_L3CACHESIZE;
  if (sysctl(nm, 2, &count, &len, nullptr, 0) == 0) {
    nL3CacheSize = count;
  }
#else
  struct sysinfo info;
  if (sysinfo(&info) == 0) {
    nPhysicalRAM = info.totalram;
  }

  nCacheLine = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

  QProcess lscpu;
  lscpu.start("lscpu", QStringList());

  if (lscpu.waitForFinished(-1)) {
    QString lscpuOutput(lscpu.readAllStandardOutput());
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    QStringList cpuInfos = lscpuOutput.split(QChar('\n'), Qt::SkipEmptyParts);
#else
    QStringList cpuInfos = lscpuOutput.split(QChar('\n'), QString::SkipEmptyParts);
#endif
    int64_t threadsPerCore = 0;
    for (const auto& cpuInfo : cpuInfos) {
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
      QStringList cInfo = cpuInfo.split(QChar(':'), Qt::SkipEmptyParts);
#else
      QStringList cInfo = cpuInfo.split(QChar(':'), QString::SkipEmptyParts);
#endif
      if (cInfo.size() == 2) {
        bool ok = false;
        if (cInfo[0] == "CPU(s)") {
          nLogicalCores = cInfo[1].toLongLong(&ok);
        } else if (cInfo[0] == "Thread(s) per core") {
          threadsPerCore = cInfo[1].toLongLong(&ok);
        } else if (cInfo[0] == "L1d cache") {
          if (cInfo[1].endsWith("K")) {
            cInfo[1].chop(1);
            nL1DCacheSize = cInfo[1].toLongLong(&ok) * 1024_u64;
          } else if (cInfo[1].endsWith("KiB")) {
            cInfo[1].chop(3);
            nL1DCacheSize = cInfo[1].toLongLong(&ok) * 1024_u64;
          } else if (cInfo[1].endsWith("MiB")) {
            cInfo[1].chop(3);
            nL1DCacheSize = cInfo[1].toLongLong(&ok) * 1024_u64 * 1024;
          }
        } else if (cInfo[0] == "L1i cache") {
          if (cInfo[1].endsWith("K")) {
            cInfo[1].chop(1);
            nL1ICacheSize = cInfo[1].toLongLong(&ok) * 1024_u64;
          } else if (cInfo[1].endsWith("KiB")) {
            cInfo[1].chop(3);
            nL1ICacheSize = cInfo[1].toLongLong(&ok) * 1024_u64;
          } else if (cInfo[1].endsWith("MiB")) {
            cInfo[1].chop(3);
            nL1ICacheSize = cInfo[1].toLongLong(&ok) * 1024_u64 * 1024;
          }
        } else if (cInfo[0] == "L2 cache") {
          if (cInfo[1].endsWith("K")) {
            cInfo[1].chop(1);
            nL2CacheSize = cInfo[1].toLongLong(&ok) * 1024_u64;
          } else if (cInfo[1].endsWith("KiB")) {
            cInfo[1].chop(3);
            nL2CacheSize = cInfo[1].toLongLong(&ok) * 1024_u64;
          } else if (cInfo[1].endsWith("MiB")) {
            cInfo[1].chop(3);
            nL2CacheSize = cInfo[1].toLongLong(&ok) * 1024_u64 * 1024;
          }
        } else if (cInfo[0] == "L3 cache") {
          if (cInfo[1].endsWith("K")) {
            cInfo[1].chop(1);
            nL3CacheSize = cInfo[1].toLongLong(&ok) * 1024_u64;
          } else if (cInfo[1].endsWith("KiB")) {
            cInfo[1].chop(3);
            nL3CacheSize = cInfo[1].toLongLong(&ok) * 1024_u64;
          } else if (cInfo[1].endsWith("MiB")) {
            cInfo[1].chop(3);
            nL3CacheSize = cInfo[1].toLongLong(&ok) * 1024_u64 * 1024;
          }
        }
      }
    }
    if (threadsPerCore != 0) {
      nPhysicalCores = nLogicalCores / threadsPerCore;
    }
  } else {
    LOG(ERROR) << lscpu.readAllStandardError();
  }
#endif
  nStdHardwareConcurrency = std::thread::hardware_concurrency();
}

} // namespace nim
