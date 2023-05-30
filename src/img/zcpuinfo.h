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

  static void runProgram(const QString& programName) ;

public:
  size_t nPhysicalCores = 1;
  size_t nLogicalCores = 1;
  size_t nStdHardwareConcurrency = 1;

  uint64_t nPhysicalRAM = 0;

  bool bMMX = false;
  bool bSSE = false;
  bool bSSE2 = false;
  bool bSSE3 = false;
  bool bSSSE3 = false;
  bool bSSE41 = false;
  bool bSSE42 = false;
  bool bAVX = false;
  bool bAVX2 = false;
  bool bMOVBE = false;

  bool bAES = false;
  bool bPCLMULQDQ = false;
  bool bRDRAND = false;
  bool bF16C = false;
  bool bRDSEED = false;
  bool bADX = false;
  bool bPREFTEHCHW = false;
  bool bSHA = false;

  bool bAVX512F = false;
  bool bAVX512DQ = false;
  bool bAVX512PF = false;
  bool bAVX512ER = false;
  bool bAVX512CD = false;
  bool bAVX512BW = false;
  bool bAVX512VL = false;
  bool bAVX512VBMI = false;
  bool bMPX = false;
  bool bAVX512_4FMADDPS = false;
  bool bAVX512_4VNNIW = false;

private:
  uint64_t m_realPhysicalRAM = 0;
};

} // namespace nim
