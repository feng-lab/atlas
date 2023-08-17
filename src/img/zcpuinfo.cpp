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

namespace {

using namespace nim;

const char* vendor_to_string(enum cpuinfo_vendor vendor)
{
  switch (vendor) {
    case cpuinfo_vendor_unknown:
      return "unknown";
    case cpuinfo_vendor_intel:
      return "Intel";
    case cpuinfo_vendor_amd:
      return "AMD";
    case cpuinfo_vendor_huawei:
      return "Huawei";
    case cpuinfo_vendor_hygon:
      return "Hygon";
    case cpuinfo_vendor_arm:
      return "ARM";
    case cpuinfo_vendor_qualcomm:
      return "Qualcomm";
    case cpuinfo_vendor_apple:
      return "Apple";
    case cpuinfo_vendor_samsung:
      return "Samsung";
    case cpuinfo_vendor_nvidia:
      return "Nvidia";
    case cpuinfo_vendor_mips:
      return "MIPS";
    case cpuinfo_vendor_ibm:
      return "IBM";
    case cpuinfo_vendor_ingenic:
      return "Ingenic";
    case cpuinfo_vendor_via:
      return "VIA";
    case cpuinfo_vendor_cavium:
      return "Cavium";
    case cpuinfo_vendor_broadcom:
      return "Broadcom";
    case cpuinfo_vendor_apm:
      return "Applied Micro";
    default:
      return nullptr;
  }
}

const char* uarch_to_string(enum cpuinfo_uarch uarch)
{
  switch (uarch) {
    case cpuinfo_uarch_unknown:
      return "unknown";
    case cpuinfo_uarch_p5:
      return "P5";
    case cpuinfo_uarch_quark:
      return "Quark";
    case cpuinfo_uarch_p6:
      return "P6";
    case cpuinfo_uarch_dothan:
      return "Dothan";
    case cpuinfo_uarch_yonah:
      return "Yonah";
    case cpuinfo_uarch_conroe:
      return "Conroe";
    case cpuinfo_uarch_penryn:
      return "Penryn";
    case cpuinfo_uarch_nehalem:
      return "Nehalem";
    case cpuinfo_uarch_sandy_bridge:
      return "Sandy Bridge";
    case cpuinfo_uarch_ivy_bridge:
      return "Ivy Bridge";
    case cpuinfo_uarch_haswell:
      return "Haswell";
    case cpuinfo_uarch_broadwell:
      return "Broadwell";
    case cpuinfo_uarch_sky_lake:
      return "Sky Lake";
    case cpuinfo_uarch_palm_cove:
      return "Palm Cove";
    case cpuinfo_uarch_sunny_cove:
      return "Sunny Cove";
    case cpuinfo_uarch_willamette:
      return "Willamette";
    case cpuinfo_uarch_prescott:
      return "Prescott";
    case cpuinfo_uarch_bonnell:
      return "Bonnell";
    case cpuinfo_uarch_saltwell:
      return "Saltwell";
    case cpuinfo_uarch_silvermont:
      return "Silvermont";
    case cpuinfo_uarch_airmont:
      return "Airmont";
    case cpuinfo_uarch_goldmont:
      return "Goldmont";
    case cpuinfo_uarch_goldmont_plus:
      return "Goldmont Plus";
    case cpuinfo_uarch_knights_ferry:
      return "Knights Ferry";
    case cpuinfo_uarch_knights_corner:
      return "Knights Corner";
    case cpuinfo_uarch_knights_landing:
      return "Knights Landing";
    case cpuinfo_uarch_knights_hill:
      return "Knights Hill";
    case cpuinfo_uarch_knights_mill:
      return "Knights Mill";
    case cpuinfo_uarch_k5:
      return "K5";
    case cpuinfo_uarch_k6:
      return "K6";
    case cpuinfo_uarch_k7:
      return "K7";
    case cpuinfo_uarch_k8:
      return "K8";
    case cpuinfo_uarch_k10:
      return "K10";
    case cpuinfo_uarch_bulldozer:
      return "Bulldozer";
    case cpuinfo_uarch_piledriver:
      return "Piledriver";
    case cpuinfo_uarch_steamroller:
      return "Steamroller";
    case cpuinfo_uarch_excavator:
      return "Excavator";
    case cpuinfo_uarch_zen:
      return "Zen";
    case cpuinfo_uarch_zen2:
      return "Zen 2";
    case cpuinfo_uarch_zen3:
      return "Zen 3";
    case cpuinfo_uarch_zen4:
      return "Zen 4";
    case cpuinfo_uarch_geode:
      return "Geode";
    case cpuinfo_uarch_bobcat:
      return "Bobcat";
    case cpuinfo_uarch_jaguar:
      return "Jaguar";
    case cpuinfo_uarch_puma:
      return "Puma";
    case cpuinfo_uarch_xscale:
      return "XScale";
    case cpuinfo_uarch_arm7:
      return "ARM7";
    case cpuinfo_uarch_arm9:
      return "ARM9";
    case cpuinfo_uarch_arm11:
      return "ARM11";
    case cpuinfo_uarch_cortex_a5:
      return "Cortex-A5";
    case cpuinfo_uarch_cortex_a7:
      return "Cortex-A7";
    case cpuinfo_uarch_cortex_a8:
      return "Cortex-A8";
    case cpuinfo_uarch_cortex_a9:
      return "Cortex-A9";
    case cpuinfo_uarch_cortex_a12:
      return "Cortex-A12";
    case cpuinfo_uarch_cortex_a15:
      return "Cortex-A15";
    case cpuinfo_uarch_cortex_a17:
      return "Cortex-A17";
    case cpuinfo_uarch_cortex_a32:
      return "Cortex-A32";
    case cpuinfo_uarch_cortex_a35:
      return "Cortex-A35";
    case cpuinfo_uarch_cortex_a53:
      return "Cortex-A53";
    case cpuinfo_uarch_cortex_a55r0:
      return "Cortex-A55r0";
    case cpuinfo_uarch_cortex_a55:
      return "Cortex-A55";
    case cpuinfo_uarch_cortex_a57:
      return "Cortex-A57";
    case cpuinfo_uarch_cortex_a65:
      return "Cortex-A65";
    case cpuinfo_uarch_cortex_a72:
      return "Cortex-A72";
    case cpuinfo_uarch_cortex_a73:
      return "Cortex-A73";
    case cpuinfo_uarch_cortex_a75:
      return "Cortex-A75";
    case cpuinfo_uarch_cortex_a76:
      return "Cortex-A76";
    case cpuinfo_uarch_cortex_a77:
      return "Cortex-A77";
    case cpuinfo_uarch_cortex_a78:
      return "Cortex-A78";
    case cpuinfo_uarch_cortex_a510:
      return "Cortex-A510";
    case cpuinfo_uarch_cortex_a710:
      return "Cortex-A710";
    case cpuinfo_uarch_cortex_a715:
      return "Cortex-A715";
    case cpuinfo_uarch_cortex_x1:
      return "Cortex-X1";
    case cpuinfo_uarch_cortex_x2:
      return "Cortex-X2";
    case cpuinfo_uarch_cortex_x3:
      return "Cortex-X3";
    case cpuinfo_uarch_neoverse_n1:
      return "Neoverse N1";
    case cpuinfo_uarch_neoverse_e1:
      return "Neoverse E1";
    case cpuinfo_uarch_neoverse_v1:
      return "Neoverse V1";
    case cpuinfo_uarch_neoverse_n2:
      return "Neoverse N2";
    case cpuinfo_uarch_scorpion:
      return "Scorpion";
    case cpuinfo_uarch_krait:
      return "Krait";
    case cpuinfo_uarch_kryo:
      return "Kryo";
    case cpuinfo_uarch_falkor:
      return "Falkor";
    case cpuinfo_uarch_saphira:
      return "Saphira";
    case cpuinfo_uarch_denver:
      return "Denver";
    case cpuinfo_uarch_denver2:
      return "Denver 2";
    case cpuinfo_uarch_carmel:
      return "Carmel";
    case cpuinfo_uarch_exynos_m1:
      return "Exynos M1";
    case cpuinfo_uarch_exynos_m2:
      return "Exynos M2";
    case cpuinfo_uarch_exynos_m3:
      return "Exynos M3";
    case cpuinfo_uarch_exynos_m4:
      return "Exynos M4";
    case cpuinfo_uarch_exynos_m5:
      return "Exynos M5";
    case cpuinfo_uarch_swift:
      return "Swift";
    case cpuinfo_uarch_cyclone:
      return "Cyclone";
    case cpuinfo_uarch_typhoon:
      return "Typhoon";
    case cpuinfo_uarch_twister:
      return "Twister";
    case cpuinfo_uarch_hurricane:
      return "Hurricane";
    case cpuinfo_uarch_monsoon:
      return "Monsoon";
    case cpuinfo_uarch_mistral:
      return "Mistral";
    case cpuinfo_uarch_vortex:
      return "Vortex";
    case cpuinfo_uarch_tempest:
      return "Tempest";
    case cpuinfo_uarch_lightning:
      return "Lightning";
    case cpuinfo_uarch_thunder:
      return "Thunder";
    case cpuinfo_uarch_firestorm:
      return "Firestorm";
    case cpuinfo_uarch_icestorm:
      return "Icestorm";
    case cpuinfo_uarch_avalanche:
      return "Avalanche";
    case cpuinfo_uarch_blizzard:
      return "Blizzard";
    case cpuinfo_uarch_thunderx:
      return "ThunderX";
    case cpuinfo_uarch_thunderx2:
      return "ThunderX2";
    case cpuinfo_uarch_pj4:
      return "PJ4";
    case cpuinfo_uarch_brahma_b15:
      return "Brahma B15";
    case cpuinfo_uarch_brahma_b53:
      return "Brahma B53";
    case cpuinfo_uarch_xgene:
      return "X-Gene";
    case cpuinfo_uarch_dhyana:
      return "Dhyana";
    case cpuinfo_uarch_taishan_v110:
      return "TaiShan v110";
    default:
      return nullptr;
  }
}

void logCPUInfo()
{
  std::string message;
#ifdef __ANDROID__
  message = fmt::format("SoC name: {}\n", cpuinfo_get_package(0)->name);
#else
  message = "Packages:\n";
  for (uint32_t i = 0; i < cpuinfo_get_packages_count(); i++) {
    message += fmt::format("\t{}: {}\n", i, cpuinfo_get_package(i)->name);
  }
#endif
  LOG(INFO) << message;
  message = "Microarchitectures:\n";
  for (uint32_t i = 0; i < cpuinfo_get_uarchs_count(); i++) {
    const struct cpuinfo_uarch_info* uarch_info = cpuinfo_get_uarch(i);
    const char* uarch_string = uarch_to_string(uarch_info->uarch);
    if (uarch_string == nullptr) {
      message += fmt::format("\t{}x Unknown (0x{:08x})\n", uarch_info->core_count, (uint32_t)uarch_info->uarch);
    } else {
      message += fmt::format("\t{}x {}\n", uarch_info->core_count, uarch_string);
    }
  }
  LOG(INFO) << message;
  message = "Cores:\n";
  for (uint32_t i = 0; i < cpuinfo_get_cores_count(); i++) {
    const struct cpuinfo_core* core = cpuinfo_get_core(i);
    message += fmt::format(
      "\t{}: {} processor{} ({}{})",
      i,
      core->processor_count,
      (core->processor_count > 1 ? "s" : ""),
      core->processor_start,
      (core->processor_count > 1 ? fmt::format("-{}", core->processor_start + core->processor_count - 1) : ""));

    const char* vendor_string = vendor_to_string(core->vendor);
    const char* uarch_string = uarch_to_string(core->uarch);
    if (vendor_string == nullptr) {
      message += fmt::format(", vendor 0x{:08x} uarch 0x{:08x}\n", (uint32_t)core->vendor, (uint32_t)core->uarch);
    } else if (uarch_string == nullptr) {
      message += fmt::format(", {} uarch 0x{:08x}\n", vendor_string, (uint32_t)core->uarch);
    } else {
      message += fmt::format(", {} {}\n", vendor_string, uarch_string);
    }
  }
  LOG(INFO) << message;
  message = "Logical processors";
#if defined(__linux__)
  message += " (System ID)";
#endif
  message += ":\n";
  for (uint32_t i = 0; i < cpuinfo_get_processors_count(); i++) {
    [[maybe_unused]] const struct cpuinfo_processor* processor = cpuinfo_get_processor(i);
    message += fmt::format("\t{}", i);

#if defined(__linux__)
    message += fmt::format(" ({})", processor->linux_id);
#endif

#if CPUINFO_ARCH_X86 || CPUINFO_ARCH_X86_64
    message += fmt::format(": APIC ID 0x{:08x}\n", processor->apic_id);
#else
    message += "\n";
#endif
  }
  LOG(INFO) << message;
}

void report_cache(uint32_t count, const struct cpuinfo_cache* cache, uint32_t level, const char* nonunified_type)
{
  const char* type = (cache->flags & CPUINFO_CACHE_UNIFIED) ? "unified" : nonunified_type;

  std::string message = fmt::format("L{} {} cache: ", level, type);

  uint32_t size = cache->size;
  const char* units = "bytes";
  if (size % (1048576_u32) == 0) {
    size /= 1048576_u32;
    units = "MB";
  } else if (size % 1024_u32 == 0) {
    size /= 1024_u32;
    units = "KB";
  }
  if (count != 1) {
    message += fmt::format("{} x ", count);
  }
  if (level == 1) {
    message += fmt::format("{} {}, ", size, units);
  } else {
    message +=
      fmt::format("{} {} ({}), ", size, units, (cache->flags & CPUINFO_CACHE_INCLUSIVE) ? "inclusive" : "exclusive");
  }

  if (cache->associativity * cache->line_size == cache->size) {
    message += "fully associative";
  } else {
    message += fmt::format("{}-way set associative", cache->associativity);
  }
  if (cache->sets != 0) {
    message += fmt::format(" ({} sets", cache->sets);
    if (cache->partitions != 1) {
      message += fmt::format(", {} partitions", cache->partitions);
    }
    if (cache->flags & CPUINFO_CACHE_COMPLEX_INDEXING) {
      message += ", complex indexing), ";
    } else {
      message += "), ";
    }
  }

  message += fmt::format("{} byte lines", cache->line_size);
  if (cache->processor_count != 0) {
    message += fmt::format(", shared by {} processors\n", cache->processor_count);
  } else {
    message += "\n";
  }

  LOG(INFO) << message;
}

void logCacheInfo()
{
  LOG(INFO) << fmt::format("Max cache size (upper bound): {} bytes", cpuinfo_get_max_cache_size());

  if (cpuinfo_get_l1i_caches_count() != 0 && (cpuinfo_get_l1i_cache(0)->flags & CPUINFO_CACHE_UNIFIED) == 0) {
    report_cache(cpuinfo_get_l1i_caches_count(), cpuinfo_get_l1i_cache(0), 1, "instruction");
  }
  if (cpuinfo_get_l1d_caches_count() != 0) {
    report_cache(cpuinfo_get_l1d_caches_count(), cpuinfo_get_l1d_cache(0), 1, "data");
  }
  if (cpuinfo_get_l2_caches_count() != 0) {
    report_cache(cpuinfo_get_l2_caches_count(), cpuinfo_get_l2_cache(0), 2, "data");
  }
  if (cpuinfo_get_l3_caches_count() != 0) {
    report_cache(cpuinfo_get_l3_caches_count(), cpuinfo_get_l3_cache(0), 3, "data");
  }
  if (cpuinfo_get_l4_caches_count() != 0) {
    report_cache(cpuinfo_get_l4_caches_count(), cpuinfo_get_l4_cache(0), 4, "data");
  }
}

void logISAInfo()
{
#if CPUINFO_ARCH_X86_64
  LOG(INFO) << fmt::format("Scalar instructions:");
  LOG(INFO) << fmt::format("\tLAHF/SAHF: {}", cpuinfo_has_x86_lahf_sahf() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tLZCNT: {}", cpuinfo_has_x86_lzcnt() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tPOPCNT: {}", cpuinfo_has_x86_popcnt() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tTBM: {}", cpuinfo_has_x86_tbm() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tBMI: {}", cpuinfo_has_x86_bmi() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tBMI2: {}", cpuinfo_has_x86_bmi2() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tADCX/ADOX: {}", cpuinfo_has_x86_adx() ? "yes" : "no");

  LOG(INFO) << fmt::format("Memory instructions:\n");
  LOG(INFO) << fmt::format("\tMOVBE: {}", cpuinfo_has_x86_movbe() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tPREFETCH: {}", cpuinfo_has_x86_prefetch() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tPREFETCHW: {}", cpuinfo_has_x86_prefetchw() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tPREFETCHWT1: {}", cpuinfo_has_x86_prefetchwt1() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tCLZERO: {}", cpuinfo_has_x86_clzero() ? "yes" : "no");

  LOG(INFO) << fmt::format("SIMD extensions:\n");
  LOG(INFO) << fmt::format("\tMMX: {}", cpuinfo_has_x86_mmx() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tMMX+: {}", cpuinfo_has_x86_mmx_plus() ? "yes" : "no");
  LOG(INFO) << fmt::format("\t3dnow!: {}", cpuinfo_has_x86_3dnow() ? "yes" : "no");
  LOG(INFO) << fmt::format("\t3dnow!+: {}", cpuinfo_has_x86_3dnow_plus() ? "yes" : "no");
  LOG(INFO) << fmt::format("\t3dnow! Geode: {}", cpuinfo_has_x86_3dnow_geode() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tDAZ: {}", cpuinfo_has_x86_daz() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tSSE: {}", cpuinfo_has_x86_sse() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tSSE2: {}", cpuinfo_has_x86_sse2() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tSSE3: {}", cpuinfo_has_x86_sse3() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tSSSE3: {}", cpuinfo_has_x86_ssse3() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tSSE4.1: {}", cpuinfo_has_x86_sse4_1() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tSSE4.2: {}", cpuinfo_has_x86_sse4_2() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tSSE4a: {}", cpuinfo_has_x86_sse4a() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tMisaligned SSE: {}", cpuinfo_has_x86_misaligned_sse() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX: {}", cpuinfo_has_x86_avx() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tFMA3: {}", cpuinfo_has_x86_fma3() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tFMA4: {}", cpuinfo_has_x86_fma4() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tXOP: {}", cpuinfo_has_x86_xop() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tF16C: {}", cpuinfo_has_x86_f16c() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX2: {}", cpuinfo_has_x86_avx2() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512F: {}", cpuinfo_has_x86_avx512f() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512PF: {}", cpuinfo_has_x86_avx512pf() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512ER: {}", cpuinfo_has_x86_avx512er() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512CD: {}", cpuinfo_has_x86_avx512cd() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512DQ: {}", cpuinfo_has_x86_avx512dq() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512BW: {}", cpuinfo_has_x86_avx512bw() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512VL: {}", cpuinfo_has_x86_avx512vl() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512IFMA: {}", cpuinfo_has_x86_avx512ifma() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512VBMI: {}", cpuinfo_has_x86_avx512vbmi() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512VBMI2: {}", cpuinfo_has_x86_avx512vbmi2() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512BITALG: {}", cpuinfo_has_x86_avx512bitalg() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512VPOPCNTDQ: {}", cpuinfo_has_x86_avx512vpopcntdq() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512VNNI: {}", cpuinfo_has_x86_avx512vnni() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512BF16: {}", cpuinfo_has_x86_avx512bf16() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512FP16: {}", cpuinfo_has_x86_avx512fp16() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512VP2INTERSECT: {}", cpuinfo_has_x86_avx512vp2intersect() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512_4VNNIW: {}", cpuinfo_has_x86_avx512_4vnniw() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tAVX512_4FMAPS: {}", cpuinfo_has_x86_avx512_4fmaps() ? "yes" : "no");

  LOG(INFO) << fmt::format("Multi-threading extensions:\n");
  LOG(INFO) << fmt::format("\tMONITOR/MWAIT: {}", cpuinfo_has_x86_mwait() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tMONITORX/MWAITX: {}", cpuinfo_has_x86_mwaitx() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tCMPXCHG16B: {}", cpuinfo_has_x86_cmpxchg16b() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tHLE: {}", cpuinfo_has_x86_hle() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tRTM: {}", cpuinfo_has_x86_rtm() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tXTEST: {}", cpuinfo_has_x86_xtest() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tRDPID: {}", cpuinfo_has_x86_rdpid() ? "yes" : "no");

  LOG(INFO) << fmt::format("Cryptography extensions:\n");
  LOG(INFO) << fmt::format("\tAES: {}", cpuinfo_has_x86_aes() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tVAES: {}", cpuinfo_has_x86_vaes() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tPCLMULQDQ: {}", cpuinfo_has_x86_pclmulqdq() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tVPCLMULQDQ: {}", cpuinfo_has_x86_vpclmulqdq() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tGFNI: {}", cpuinfo_has_x86_gfni() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tRDRAND: {}", cpuinfo_has_x86_rdrand() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tRDSEED: {}", cpuinfo_has_x86_rdseed() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tSHA: {}", cpuinfo_has_x86_sha() ? "yes" : "no");

  LOG(INFO) << fmt::format("Profiling instructions:\n");
  LOG(INFO) << fmt::format("\tRDTSCP: {}", cpuinfo_has_x86_rdtscp() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tMPX: {}", cpuinfo_has_x86_mpx() ? "yes" : "no");

  LOG(INFO) << fmt::format("System instructions:\n");
  LOG(INFO) << fmt::format("\tCLWB: {}", cpuinfo_has_x86_clwb() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tFXSAVE/FXSTOR: {}", cpuinfo_has_x86_fxsave() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tXSAVE/XSTOR: {}", cpuinfo_has_x86_xsave() ? "yes" : "no");
#endif /* CPUINFO_ARCH_X86 || CPUINFO_ARCH_X86_64 */

#if CPUINFO_ARCH_ARM
  LOG(INFO) << fmt::format("Instruction sets:\n");
  LOG(INFO) << fmt::format("\tThumb: {}", cpuinfo_has_arm_thumb() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tThumb 2: {}", cpuinfo_has_arm_thumb2() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARMv5E: {}", cpuinfo_has_arm_v5e() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARMv6: {}", cpuinfo_has_arm_v6() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARMv6-K: {}", cpuinfo_has_arm_v6k() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARMv7: {}", cpuinfo_has_arm_v7() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARMv7 MP: {}", cpuinfo_has_arm_v7mp() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARMv8: {}", cpuinfo_has_arm_v8() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tIDIV: {}", cpuinfo_has_arm_idiv() ? "yes" : "no");

  LOG(INFO) << fmt::format("Floating-Point support:\n");
  LOG(INFO) << fmt::format("\tVFPv2: {}", cpuinfo_has_arm_vfpv2() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tVFPv3: {}", cpuinfo_has_arm_vfpv3() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tVFPv3+D32: {}", cpuinfo_has_arm_vfpv3_d32() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tVFPv3+FP16: {}", cpuinfo_has_arm_vfpv3_fp16() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tVFPv3+FP16+D32: {}", cpuinfo_has_arm_vfpv3_fp16_d32() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tVFPv4: {}", cpuinfo_has_arm_vfpv4() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tVFPv4+D32: {}", cpuinfo_has_arm_vfpv4_d32() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tVJCVT: {}", cpuinfo_has_arm_jscvt() ? "yes" : "no");

  LOG(INFO) << fmt::format("SIMD extensions:\n");
  LOG(INFO) << fmt::format("\tWMMX: {}", cpuinfo_has_arm_wmmx() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tWMMX 2: {}", cpuinfo_has_arm_wmmx2() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tNEON: {}", cpuinfo_has_arm_neon() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tNEON-FP16: {}", cpuinfo_has_arm_neon_fp16() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tNEON-FMA: {}", cpuinfo_has_arm_neon_fma() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tNEON VQRDMLAH/VQRDMLSH: {}", cpuinfo_has_arm_neon_rdm() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tNEON FP16 arithmetics: {}", cpuinfo_has_arm_neon_fp16_arith() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tNEON complex: {}", cpuinfo_has_arm_fcma() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tNEON VSDOT/VUDOT: {}", cpuinfo_has_arm_neon_dot() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tNEON VFMLAL/VFMLSL: {}", cpuinfo_has_arm_fhm() ? "yes" : "no");

  LOG(INFO) << fmt::format("Cryptography extensions:\n");
  LOG(INFO) << fmt::format("\tAES: {}", cpuinfo_has_arm_aes() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tSHA1: {}", cpuinfo_has_arm_sha1() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tSHA2: {}", cpuinfo_has_arm_sha2() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tPMULL: {}", cpuinfo_has_arm_pmull() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tCRC32: {}", cpuinfo_has_arm_crc32() ? "yes" : "no");
#endif /* CPUINFO_ARCH_ARM */

#if CPUINFO_ARCH_ARM64
  LOG(INFO) << fmt::format("Instruction sets:\n");
  LOG(INFO) << fmt::format("\tARM v8.1 atomics: {}", cpuinfo_has_arm_atomics() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARM v8.1 SQRDMLxH: {}", cpuinfo_has_arm_neon_rdm() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARM v8.2 FP16 arithmetics: {}", cpuinfo_has_arm_fp16_arith() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARM v8.2 FHM: {}", cpuinfo_has_arm_fhm() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARM v8.2 BF16: {}", cpuinfo_has_arm_bf16() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARM v8.2 Int8 dot product: {}", cpuinfo_has_arm_neon_dot() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARM v8.2 Int8 matrix multiplication: {}", cpuinfo_has_arm_i8mm() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARM v8.3 JS conversion: {}", cpuinfo_has_arm_jscvt() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARM v8.3 complex: {}", cpuinfo_has_arm_fcma() ? "yes" : "no");

  LOG(INFO) << fmt::format("SIMD extensions:\n");
  LOG(INFO) << fmt::format("\tARM SVE: {}", cpuinfo_has_arm_sve() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tARM SVE 2: {}", cpuinfo_has_arm_sve2() ? "yes" : "no");

  LOG(INFO) << fmt::format("Cryptography extensions:\n");
  LOG(INFO) << fmt::format("\tAES: {}", cpuinfo_has_arm_aes() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tSHA1: {}", cpuinfo_has_arm_sha1() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tSHA2: {}", cpuinfo_has_arm_sha2() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tPMULL: {}", cpuinfo_has_arm_pmull() ? "yes" : "no");
  LOG(INFO) << fmt::format("\tCRC32: {}", cpuinfo_has_arm_crc32() ? "yes" : "no");
#endif
}

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
  logCPUInfo();
  logCacheInfo();
  logISAInfo();
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

  bAVX = cpuinfo_has_x86_avx();
  bAVX2 = cpuinfo_has_x86_avx2();

//  bAVX512F = cpuinfo_has_x86_avx512f();
//  bAVX512BW = cpuinfo_has_x86_avx512bw();
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
