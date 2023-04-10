#pragma once

#include "zglobal.h"
#include <cstdint>

namespace nim {

class ZCpuInfo
{
public:
  static ZCpuInfo& instance();

  ZCpuInfo();

  // log useful cpu info
  void logCpuInfo() const;

  // set to limit memory usage
  void setMemoryLimitInBytes(uint64_t n);

protected:
  void detectCpuInfo();

  void detectCoreAndThreadNumber();

public:
  QString vendor;
  QString brand;

  int32_t nPhysicalCores = 1;
  int32_t nLogicalCores = 1;
  size_t nStdHardwareConcurrency = 1;

  uint64_t nCacheLine = 0;
  uint64_t nL1ICacheSize = 0;
  uint64_t nL1DCacheSize = 0;
  uint64_t nL2CacheSize = 0;
  uint64_t nL3CacheSize = 0;
  uint64_t nPhysicalRAM = 0;

  bool bXOP = false;
  bool bFMA = false;
  bool bFMA4 = false;
  bool b3DNow = false;
  bool b3DNowExt = false;
  bool bMMX = false;
  bool bMMXExtensions = false;
  bool bSSE = false;
  bool bSSE2 = false;
  bool bSSE3 = false;
  bool bSSSE3 = false;
  bool bSSE41 = false;
  bool bSSE42 = false;
  bool bSSE4A = false;
  bool bAVX = false;
  bool bAVX2 = false;
  bool bBMI = false;
  bool bMOVBE = false;

  bool bCMPXCHG16B = false;
  bool bPOPCNT = false;
  bool bLZCNT = false;
  bool bABM = false; // for AMD
  bool bRDTSCP = false;

  bool bAESNI = false;
  bool bPCLMULQDQ = false;
  bool bRDRAND = false;
  bool bF16C = false;
  bool bRDSEED = false;
  bool bADX = false;
  bool bPREFTEHCHW = false;
  bool bSHA = false;

  bool bDTES64 = false;
  bool bMONITOR = false;
  bool bDSCPL = false;
  bool bVMX = false;
  bool bSMX = false;
  bool bEIST = false;
  bool bTM2 = false;
  bool bCNXTID = false;
  bool bSDBG = false;
  bool bxTPRUpdateControl = false;
  bool bPDCM = false;
  bool bPCID = false;
  bool bDCA = false;
  bool bx2APIC = false;
  bool bTSCDeadline = false;
  bool bXSAVE = false;
  bool bOSXSAVE = false;

  bool bFPU = false;
  bool bVME = false;
  bool bDE = false;
  bool bPSE = false;
  bool bTSC = false;
  bool bMSR = false;
  bool bPAE = false;
  bool bMCE = false;
  bool bCMPXCHG8B = false;
  bool bAPIC = false;
  bool bSEP = false;
  bool bMTRR = false;
  bool bPGE = false;
  bool bMCA = false;
  bool bCMOV = false;
  bool bPAT = false;
  bool bPSE36 = false;
  bool bPSN = false;
  bool bCLFSH = false;
  bool bDS = false;
  bool bACPI = false;
  bool bFXSR = false;
  bool bSS = false;
  bool bHTT = false;
  bool bTM = false;
  bool bPBE = false;

  bool bAVX512F = false;
  bool bAVX512DQ = false;
  bool bAVX512PF = false;
  bool bAVX512ER = false;
  bool bAVX512CD = false;
  bool bAVX512BW = false;
  bool bAVX512VL = false;

  bool bPREFTEHCHWT1 = false;

  int32_t nSteppingID = 0;
  int32_t nModel = 0;
  int32_t nFamily = 0;
  int32_t nProcessorType = 0;
  int32_t nExtendedmodel = 0;
  int32_t nExtendedfamily = 0;
  int32_t nMaxLogicalProcessors = 1;
  int32_t nAPICPhysicalID = 0;

private:
  uint64_t m_realPhysicalRAM = 0;
};

} // namespace nim
