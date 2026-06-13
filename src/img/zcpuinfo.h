#pragma once

#include <QString>
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

  bool isX86_64 = false;

  bool bAVX = false;
  bool bAVX2 = false;

private:
  uint64_t m_realPhysicalRAM = 0;
};

} // namespace nim
