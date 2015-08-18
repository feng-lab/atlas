#ifndef ZCPUINFO_H
#define ZCPUINFO_H

#include <QString>
#include <stdint.h>

namespace nim {

#define ZCpuInfoInstance nim::ZCpuInfo::instance()

class ZCpuInfo
{
public:
  static ZCpuInfo& instance();

  ZCpuInfo();
  ~ZCpuInfo() {}

  // log useful cpu info
  void logCpuInfo() const;

protected:
  void detectCpuInfo();
  void detectCoreAndThreadNumber();

public:
  QString sCPU;
  QString sCPUBrand;

  int nPhysicalCores;
  int nLogicalCores;

  uint64_t nCacheLine;
  uint64_t nL1ICacheSize;
  uint64_t nL1DCacheSize;
  uint64_t nL2CacheSize;
  uint64_t nL3CacheSize;
  uint64_t nPhysicalRAM;

  bool b64Available;
  bool bXOP;
  bool bFMA;
  bool bFMA4;
  bool b3DNow;
  bool b3DNowExt;
  bool bMMX;
  bool bMMXExtensions;
  bool bSSE;
  bool bSSE2;
  bool bSSE3;
  bool bSSSE3;
  bool bSSE41;
  bool bSSE42;
  bool bSSE4A;
  bool bAVX;
  bool bAVX2;
  bool bBMI;
  bool bMOVBE;

  bool bCMPXCHG8B;
  bool bCMPXCHG16B;
  bool bPOPCNT;
  bool bLZCNT;
  bool bRDTSCP;

  bool bHTT;

  int nSteppingID;
  int nModel;
  int nFamily;
  int nProcessorType;
  int nExtendedmodel;
  int nExtendedfamily;
  int nMaxLogicalProcessors;
  int nAPICPhysicalID;
};

} // namespace nim

#endif // ZCPUINFO_H
