#include "zcpuinfo.h"

#include "zlog.h"
#include "zimginterface.h"
#include <QProcess>
#include <QFile>
#include <cpuinfo.h>
#include <thread>

#ifdef _WIN32
#include "zwindowsheader.h"
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <sys/sysinfo.h>
#endif

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
  runProgram("cpu-info");
  runProgram("cache-info");
  runProgram("isa-info");
#if 0
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
#endif

  LOG(INFO) << "Number of Cores: " << nPhysicalCores;
  LOG(INFO) << "Number of Threads: " << nLogicalCores;
  LOG(INFO) << "std thread hardware_concurrency: " << nStdHardwareConcurrency;
  LOG(INFO) << "RAM: " << nPhysicalRAM;
}

void ZCpuInfo::setMemoryLimitInBytes(uint64_t n)
{
  if (n > m_realPhysicalRAM) {
    LOG(INFO) << fmt::format("memory limit {} larger than real physical memory {}, ignore", n, m_realPhysicalRAM);
    nPhysicalRAM = m_realPhysicalRAM;
    return;
  }
  nPhysicalRAM = n;
  LOG(INFO) << fmt::format("set memory usage limit to {} bytes", nPhysicalRAM);
}

void ZCpuInfo::detectCpuInfo()
{
#if CPUINFO_ARCH_X86_64
  isX86_64 = true;

  bMMX = cpuinfo_has_x86_mmx();
  bSSE = cpuinfo_has_x86_sse();;
  bSSE2 = cpuinfo_has_x86_sse2();
  bSSE3 = cpuinfo_has_x86_sse3();
  bSSSE3 = cpuinfo_has_x86_ssse3();
  bSSE41 = cpuinfo_has_x86_sse4_1();
  bSSE42 = cpuinfo_has_x86_sse4_2();
  bAVX = cpuinfo_has_x86_avx();
  bAVX2 = cpuinfo_has_x86_avx2();
  bMOVBE = cpuinfo_has_x86_movbe();

  bAES = cpuinfo_has_x86_aes();
  bPCLMULQDQ = cpuinfo_has_x86_pclmulqdq();
  bRDRAND = cpuinfo_has_x86_rdrand();;
  bF16C = cpuinfo_has_x86_f16c();;
  bRDSEED = cpuinfo_has_x86_rdseed();;
  bADX = cpuinfo_has_x86_adx();;
  bPREFTEHCHW = cpuinfo_has_x86_prefetchw();;
  bSHA = cpuinfo_has_x86_sha();;

  bAVX512F = cpuinfo_has_x86_avx512f();
  bAVX512DQ = cpuinfo_has_x86_avx512dq();
  bAVX512PF = cpuinfo_has_x86_avx512pf();
  bAVX512ER = cpuinfo_has_x86_avx512er();
  bAVX512CD = cpuinfo_has_x86_avx512cd();
  bAVX512BW = cpuinfo_has_x86_avx512bw();
  bAVX512VL = cpuinfo_has_x86_avx512vl();
  bAVX512VBMI = cpuinfo_has_x86_avx512vbmi();
  bMPX = cpuinfo_has_x86_mpx();
  bAVX512_4FMADDPS = cpuinfo_has_x86_avx512_4fmaps();
  bAVX512_4VNNIW = cpuinfo_has_x86_avx512_4vnniw();
#endif

#if CPUINFO_ARCH_ARM64
  bAVX2 = true;
#endif

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
#elif defined(__APPLE__)
  int nm[2];
  auto len = sizeof(nPhysicalRAM);
  nm[0] = CTL_HW;
  nm[1] = HW_MEMSIZE;
  if (sysctl(nm, 2, &nPhysicalRAM, &len, nullptr, 0) != 0) {
    nPhysicalRAM = 0;
  }
#else
  struct sysinfo info;
  if (sysinfo(&info) == 0) {
    nPhysicalRAM = info.totalram;
  }
#endif
  nStdHardwareConcurrency = std::thread::hardware_concurrency();

  m_realPhysicalRAM = nPhysicalRAM;
}

void ZCpuInfo::runProgram(const QString& programName)
{
#ifdef _WIN32
  QString program = ZImgGlobal::instance().resourcesDIR + QString("/") + programName + QString(".exe");
#else
  QString program = ZImgGlobal::instance().resourcesDIR + QString("/") + programName;
#endif
  if (!QFile::exists(program)) {
    LOG(ERROR) << "can not find program in resources folder " << ZImgGlobal::instance().resourcesDIR;
    return;
  }
  LOG(INFO) << program;
  QProcess process;
  process.start(program, QStringList());
  if (!process.waitForStarted()) {
    LOG(ERROR) << "can not start " << programName;
    return;
  }
  if (!process.waitForFinished(-1)) {
    LOG(ERROR) << programName << " error:";
    LOG(ERROR) << process.readAllStandardError();
    return;
  }

  LOG(INFO) << process.readAllStandardOutput();
}

} // namespace nim
