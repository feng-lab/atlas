#include "zcpuinfo.h"

#include "zbitset.h"
#include "QsLog.h"
#include <QProcess>
#include <QThread>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/param.h>
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

// http://stackoverflow.com/questions/6121792/how-to-check-if-a-cpu-supports-the-sse3-instruction-set
// http://msdn.microsoft.com/en-us/library/hskdteyh%28v=vs.90%29.aspx
// @Mysticial

namespace {

#if defined(_MSC_VER)

#include <intrin.h>

typedef BOOL (WINAPI *LPFN_GLPI)(
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION,
    PDWORD);


// Helper function to count set bits in the processor mask.
DWORD CountSetBits(ULONG_PTR bitMask)
{
    DWORD LSHIFT = sizeof(ULONG_PTR)*8 - 1;
    DWORD bitSetCount = 0;
    ULONG_PTR bitTest = (ULONG_PTR)1 << LSHIFT;
    DWORD i;

    for (i = 0; i <= LSHIFT; ++i)
    {
        bitSetCount += ((bitMask & bitTest)?1:0);
        bitTest/=2;
    }

    return bitSetCount;
}

#else

#if defined(__pic__) && defined(__i386__)

inline void __cpuid(int cpu_info[4], int info_type)
{
  __asm__ volatile (
    "mov %%ebx, %%edi\n"
    "cpuid\n"
    "xchg %%edi, %%ebx\n"
    : "=a"(cpu_info[0]), "=D"(cpu_info[1]), "=c"(cpu_info[2]), "=d"(cpu_info[3])
    : "a"(info_type)
  );
}

inline void __cpuidex(int cpu_info[4], int info_type, int info_index)
{
  __asm__ volatile (
    "mov %%ebx, %%edi\n"
    "cpuid\n"
    "xchg %%edi, %%ebx\n"
    : "=a"(cpu_info[0]), "=D"(cpu_info[1]), "=c"(cpu_info[2]), "=d"(cpu_info[3])
    : "a"(info_type), "c"(info_index)
  );
}

#else

inline void __cpuid(int cpu_info[4], int info_type)
{
  __asm__ volatile (
    "cpuid \n\t"
    : "=a"(cpu_info[0]), "=b"(cpu_info[1]), "=c"(cpu_info[2]), "=d"(cpu_info[3])
    : "a"(info_type)
  );
}

inline void __cpuidex(int cpu_info[4], int info_type, int info_index)
{
  __asm__ volatile (
    "cpuid \n\t"
    : "=a"(cpu_info[0]), "=b"(cpu_info[1]), "=c"(cpu_info[2]), "=d"(cpu_info[3])
    : "a"(info_type), "c"(info_index)
  );
}

inline uint64_t _xgetbv(unsigned int index)
{
  unsigned int eax, edx;
  __asm__ __volatile__ ("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
  return (static_cast<uint64_t>(edx) << 32) | eax;
}

#endif

#endif

}

namespace nim {

ZCpuInfo &ZCpuInfo::instance()
{
  static ZCpuInfo cpuInfo;
  return cpuInfo;
}

ZCpuInfo::ZCpuInfo()
{
  nPhysicalCores = 1;
  nLogicalCores = 1;

  nCacheLine = 0;
  nL1ICacheSize = 0;
  nL1DCacheSize = 0;
  nL2CacheSize = 0;
  nL3CacheSize = 0;
  nPhysicalRAM = 0;

  b64Available = false;
  bXOP = false;
  bFMA = false;
  bFMA4 = false;
  b3DNowExt = false;
  b3DNow = false;
  bMMX = false;
  bMMXExtensions = false;
  bSSE = false;
  bSSE2 = false;
  bSSE3 = false;
  bSSSE3 = false;
  bSSE41 = false;
  bSSE42 = false;
  bSSE4A = false;
  bAVX = false;
  bAVX2 = false;
  bBMI = false;
  bMOVBE = false;

  bCMPXCHG8B = false;
  bCMPXCHG16B = false;
  bPOPCNT = false;
  bLZCNT = false;
  bRDTSCP = false;

  bHTT = false;

  nSteppingID = 0;
  nModel = 0;
  nFamily = 0;
  nProcessorType = 0;
  nExtendedmodel = 0;
  nExtendedfamily = 0;
  nMaxLogicalProcessors = 0;
  nAPICPhysicalID = 0;

  detectCpuInfo();
}

void ZCpuInfo::logCpuInfo() const
{
  LINFO() << "CPU String:" << sCPU;
  LINFO() << "CPU Brand String:" << sCPUBrand;
  LINFO() << "Stepping ID:" << nSteppingID << " Model ID:" << nModel << " Family ID:" << nFamily << " Type:" << nProcessorType
          << " Ext.Model ID:" << nExtendedmodel << " Ext.Family ID:" << nExtendedfamily;
  LINFO() << "Number of Cores:" << nPhysicalCores;
  LINFO() << "Number of Threads:" << nLogicalCores;
  LINFO() << "Cache Line:" << nCacheLine;
  LINFO() << "L1ICache:" << nL1ICacheSize;
  LINFO() << "L1DCache:" << nL1DCacheSize;
  LINFO() << "L2Cache:" << nL2CacheSize;
  LINFO() << "L3Cache:" << nL3CacheSize;
  LINFO() << "RAM:" << nPhysicalRAM;

  QString instructions = QString(b64Available ? "64 bit Technology" : "") +
      QString(bXOP ? "; XOP" : "") +
      QString(bFMA ? "; FMA" : "") +
      QString(bFMA4 ? "; FMA4" : "") +
      QString(b3DNow ? "; 3Dnow!" : "") +
      QString(b3DNowExt ? "; 3Dnow ext" : "") +
      QString(bMMX ? "; MMX" : "") +
      QString(bMMXExtensions ? "; MMX ext" : "") +
      QString(bSSE ? "; SSE" : "") +
      QString(bSSE2 ? "; SSE2" : "") +
      QString(bSSE3 ? "; SSE3" : "") +
      QString(bSSSE3 ? "; SSSE3" : "") +
      QString(bSSE41 ? "; SSE41" : "") +
      QString(bSSE42 ? "; SSE42" : "") +
      QString(bSSE4A ? "; SSE4A" : "") +
      QString(bAVX ? "; AVX" : "") +
      QString(bAVX2 ? "; AVX2" : "") +
      QString(bBMI ? "; BMI" : "") +
      QString(bMOVBE ? "; MOVBE" : "");

  LINFO() << instructions;

  instructions = QString(bCMPXCHG8B ? "CMPXCHG8B" : "") +
      QString(bCMPXCHG16B ? "; CMPXCHG16B" : "") +
      QString(bPOPCNT ? "; POPCNT" : "") +
      QString(bLZCNT ? "; LZCNT" : "") +
      QString(bRDTSCP ? "; RDTSCP" : "");

  LINFO() << instructions;

  LINFO() << "";
}

void ZCpuInfo::detectCpuInfo()
{
  char CPUString[0x20];
  memset(CPUString, 0, sizeof(CPUString));
  char CPUBrandString[0x40];
  memset(CPUBrandString, 0, sizeof(CPUBrandString));
  int CPUInfo[4] = {-1};

  // __cpuid with an InfoType argument of 0 returns the number of
  // valid Ids in CPUInfo[0] and the CPU identification string in
  // the other three array elements. The CPU identification string is
  // not in linear order. The code below arranges the information
  // in a human readable form.
  __cpuid(CPUInfo, 0);
  uint32_t nIds = CPUInfo[0];

  *((int*)CPUString) = CPUInfo[1];
  *((int*)(CPUString+4)) = CPUInfo[3];
  *((int*)(CPUString+8)) = CPUInfo[2];
  sCPU = CPUString;

  bool osXSAVE = false;

  // Get the information associated with each valid Id
  for (uint32_t i=1; i<=nIds; ++i) {
    __cpuidex(CPUInfo, i, 0);

    std::bitset<32> eax(CPUInfo[0]);
    std::bitset<32> ebx(CPUInfo[1]);
    std::bitset<32> ecx(CPUInfo[2]);
    std::bitset<32> edx(CPUInfo[3]);
    // Interpret CPU feature information.
    if  (i == 1) {
      bCMPXCHG8B = edx[8];
      bMMX = edx[23];
      bSSE = edx[25];
      bSSE2 = edx[26];
      bHTT = edx[28];

      bSSE3 = ecx[0];
      bSSSE3 = ecx[9];
      bFMA = ecx[12];
      bCMPXCHG16B = ecx[13];
      bSSE41 = ecx[19];
      bSSE42 = ecx[20];
      bMOVBE = ecx[22];
      bPOPCNT = ecx[23];
      osXSAVE = ecx[27];
      bAVX = ecx[28];

      nSteppingID = bitsetRangeToValue(eax, 0, 4);
      nModel = bitsetRangeToValue(eax, 4, 8);
      nFamily = bitsetRangeToValue(eax, 8, 12);
      nProcessorType = bitsetRangeToValue(eax, 12, 14);
      nExtendedmodel = bitsetRangeToValue(eax, 16, 20);
      nExtendedfamily = bitsetRangeToValue(eax, 20, 28);

      nMaxLogicalProcessors = bHTT ? bitsetRangeToValue(ebx, 16, 24) : 1;
      nAPICPhysicalID = bitsetRangeToValue(ebx, 24, 32);
    }

    if (i == 7) {
      bAVX2 = ebx[5];
      bBMI = ebx[3] && ebx[8];
    }
  }

  bool hasYMM = false;
  if (osXSAVE) {
    uint64_t xcrFeatureMask = _xgetbv(0);
    hasYMM = (xcrFeatureMask & 0x6) == 6;
  }
  bAVX = bAVX && hasYMM;
  bAVX2 = bAVX2 && hasYMM;
  bFMA = bFMA && hasYMM;

  // Calling __cpuid with 0x80000000 as the InfoType argument
  // gets the number of valid extended IDs.
  __cpuid(CPUInfo, 0x80000000);
  uint32_t nExIds = CPUInfo[0];

  // Get the information associated with each extended ID.
  for (uint32_t i=0x80000001; i<=nExIds; ++i) {
    __cpuid(CPUInfo, i);

    std::bitset<32> eax(CPUInfo[0]);
    //std::bitset<32> ebx(CPUInfo[1]);
    std::bitset<32> ecx(CPUInfo[2]);
    std::bitset<32> edx(CPUInfo[3]);

    if  (i == 0x80000001) {
      bLZCNT = ecx[5];
      bSSE4A = ecx[6];
      bXOP   = ecx[11];
      bFMA4  = ecx[16];

      bMMXExtensions = edx[22];
      bRDTSCP = edx[27];
      b64Available = edx[29];
      b3DNowExt = edx[30];
      b3DNow = edx[31];
    }

    // Interpret CPU brand string and cache information.
    if  (i == 0x80000002)
      memcpy(CPUBrandString, CPUInfo, sizeof(CPUInfo));
    else if  (i == 0x80000003)
      memcpy(CPUBrandString + 16, CPUInfo, sizeof(CPUInfo));
    else if  (i == 0x80000004)
      memcpy(CPUBrandString + 32, CPUInfo, sizeof(CPUInfo));

    sCPUBrand = CPUBrandString;
  }

  detectCoreAndThreadNumber();
}

void ZCpuInfo::detectCoreAndThreadNumber()
{
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

  glpi = (LPFN_GLPI) GetProcAddress(
        GetModuleHandle(TEXT("kernel32")),
        "GetLogicalProcessorInformation");
  if (!glpi)
  {
    LERROR() << "GetLogicalProcessorInformation is not supported.";
    nPhysicalCores = QThread::idealThreadCount();
    if (nPhysicalCores < 0) {
      nPhysicalCores = 1;
      nLogicalCores = 1;
    } else {
      nLogicalCores = nPhysicalCores;
    }
    return;
  }

  while (!done)
  {
    DWORD rc = glpi(buffer, &returnLength);

    if (FALSE == rc)
    {
      if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
      {
        if (buffer)
          free(buffer);

        buffer = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(
              returnLength);

        if (!buffer)
        {
          LERROR() << "Allocation PSYSTEM_LOGICAL_PROCESSOR_INFORMATION failure";
          nPhysicalCores = QThread::idealThreadCount();
          if (nPhysicalCores < 0) {
            nPhysicalCores = 1;
            nLogicalCores = 1;
          } else {
            nLogicalCores = nPhysicalCores;
          }
          return;
        }
      }
      else
      {
        LERROR() << "Error" << GetLastError();
        nPhysicalCores = QThread::idealThreadCount();
        if (nPhysicalCores < 0) {
          nPhysicalCores = 1;
          nLogicalCores = 1;
        } else {
          nLogicalCores = nPhysicalCores;
        }
        return;
      }
    }
    else
    {
      done = TRUE;
    }
  }

  ptr = buffer;

  while (byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnLength)
  {
    switch (ptr->Relationship)
    {
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
      if (Cache->Level == 1)
      {
        processorL1CacheCount++;
        if (Cache->Type == CacheInstruction) {
          nL1ICacheSize = std::max(nL1ICacheSize, static_cast<uint64_t>(Cache->Size));
        } else if (Cache->Type == CacheData) {
          nL1DCacheSize = std::max(nL1DCacheSize, static_cast<uint64_t>(Cache->Size));
        }
      }
      else if (Cache->Level == 2)
      {
        processorL2CacheCount++;
        if (Cache->Type == CacheUnified) {
          nL2CacheSize = std::max(nL2CacheSize, static_cast<uint64_t>(Cache->Size));
        }
      }
      else if (Cache->Level == 3)
      {
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
      LERROR() << "Unsupported LOGICAL_PROCESSOR_RELATIONSHIP value.";
      break;
    }
    byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    ptr++;
  }

  LINFO() << "Number of NUMA nodes:" << numaNodeCount;
  LINFO() << "Number of physical processor packages:" << processorPackageCount;
  nPhysicalCores = processorCoreCount;
  nLogicalCores = logicalProcessorCount;

  free(buffer);
#elif defined(__APPLE__)
  size_t oldlenp = sizeof(nPhysicalCores);
  if (sysctlbyname("hw.physicalcpu_max", &nPhysicalCores, &oldlenp, nullptr, 0) != 0) {
    nPhysicalCores = QThread::idealThreadCount();
    if (nPhysicalCores < 0) {
      nPhysicalCores = 1;
      nLogicalCores = 1;
    } else {
      nLogicalCores = nPhysicalCores;
    }
  } else {
    oldlenp = sizeof(nLogicalCores);
    if (sysctlbyname("hw.logicalcpu_max", &nLogicalCores, &oldlenp, nullptr, 0) != 0) {
      nLogicalCores = QThread::idealThreadCount();
      if (nLogicalCores < 0) {
        nLogicalCores = 1;
      }
    }
  }

  int nm[2];
  size_t len = sizeof(nPhysicalRAM);

  nm[0] = CTL_HW; nm[1] = HW_MEMSIZE;
  if (sysctl(nm, 2, &nPhysicalRAM, &len, nullptr, 0) != 0)
    nPhysicalRAM = 0;

  int count = 0;
  len = sizeof(count);
  nm[0] = CTL_HW; nm[1] = HW_CACHELINE;
  if (sysctl(nm, 2, &count, &len, nullptr, 0) == 0) {
    nCacheLine = count;
  }

  count = 0;
  nm[0] = CTL_HW; nm[1] = HW_L1ICACHESIZE;
  if (sysctl(nm, 2, &count, &len, nullptr, 0) == 0) {
    nL1ICacheSize = count;
  }

  count = 0;
  nm[0] = CTL_HW; nm[1] = HW_L1DCACHESIZE;
  if (sysctl(nm, 2, &count, &len, nullptr, 0) == 0) {
    nL1DCacheSize = count;
  }

  count = 0;
  nm[0] = CTL_HW; nm[1] = HW_L2CACHESIZE;
  if (sysctl(nm, 2, &count, &len, nullptr, 0) == 0) {
    nL2CacheSize = count;
  }

  count = 0;
  nm[0] = CTL_HW; nm[1] = HW_L3CACHESIZE;
  if (sysctl(nm, 2, &count, &len, nullptr, 0) == 0) {
    nL3CacheSize = count;
  }
#else
  nPhysicalCores = QThread::idealThreadCount();
  if (nPhysicalCores < 0) {
    nPhysicalCores = 1;
    nLogicalCores = 1;
  } else {
    nLogicalCores = nPhysicalCores;
  }
#endif
}

} // namespace nim
