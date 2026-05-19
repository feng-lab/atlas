#include "zgpusysteminfo.h"

#include "zlog.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <optional>

namespace nim {

namespace {

bool isDrmCardDirectoryName(const QString& name)
{
  if (!name.startsWith("card") || name.size() == 4) {
    return false;
  }
  for (qsizetype i = 4; i < name.size(); ++i) {
    if (!name[i].isDigit()) {
      return false;
    }
  }
  return true;
}

std::optional<uint64_t> readUnsignedIntegerFile(const QString& path)
{
  if (!QFile::exists(path)) {
    return std::nullopt;
  }

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    VLOG(1) << "Failed to open GPU memory sysfs file: " << path;
    return std::nullopt;
  }

  bool ok = false;
  const qulonglong value = file.readAll().trimmed().toULongLong(&ok);
  if (!ok) {
    VLOG(1) << "Failed to parse GPU memory sysfs file: " << path;
    return std::nullopt;
  }
  return static_cast<uint64_t>(value);
}

QString drmDriverName(const QString& cardPath)
{
  const QFileInfo driverInfo(cardPath + QStringLiteral("/device/driver"));
  if (!driverInfo.exists() || !driverInfo.isSymLink()) {
    return {};
  }
  return QFileInfo(driverInfo.symLinkTarget()).fileName();
}

bool hasPciDeviceIdentity(const QString& cardPath)
{
  return QFile::exists(cardPath + QStringLiteral("/device/vendor")) &&
         QFile::exists(cardPath + QStringLiteral("/device/device"));
}

bool hasRenderNode(const QString& cardPath)
{
  QDir drmDeviceDir(cardPath + QStringLiteral("/device/drm"));
  const QStringList renderNodes =
    drmDeviceDir.entryList(QStringList() << QStringLiteral("renderD*"), QDir::AllEntries | QDir::NoDotAndDotDot);
  return !renderNodes.isEmpty();
}

bool isObviousVirtualOrSimpleDriver(const QString& driverName)
{
  const QString driver = driverName.toLower();
  return driver.contains(QStringLiteral("virtio")) || driver == QStringLiteral("simpledrm") ||
         driver == QStringLiteral("vkms") || driver == QStringLiteral("vgem") || driver == QStringLiteral("qxl") ||
         driver == QStringLiteral("bochs-drm") || driver == QStringLiteral("bochs_drm") ||
         driver == QStringLiteral("cirrus") || driver == QStringLiteral("vboxvideo") ||
         driver == QStringLiteral("vmwgfx");
}

struct DrmMemoryCandidate
{
  QString cardName;
  QString vramTotalPath;
  QString driverName;
  uint64_t capacityBytes = 0;
  bool hasPciIdentity = false;
  bool hasRenderNode = false;
  bool isVirtualOrSimple = false;

  [[nodiscard]] int rank() const
  {
    if (hasPciIdentity && hasRenderNode && !isVirtualOrSimple) {
      return 3;
    }
    if (hasRenderNode && !isVirtualOrSimple) {
      return 2;
    }
    if (!isVirtualOrSimple) {
      return 1;
    }
    return 0;
  }
};

bool isPreferredCandidate(const DrmMemoryCandidate& candidate, const DrmMemoryCandidate& current)
{
  const int candidateRank = candidate.rank();
  const int currentRank = current.rank();
  if (candidateRank != currentRank) {
    return candidateRank > currentRank;
  }
  return candidate.capacityBytes > current.capacityBytes;
}

} // namespace

ZGpuMemoryCapacityInfo detectSystemGpuMemoryCapacity()
{
  ZGpuMemoryCapacityInfo info;
  std::optional<DrmMemoryCandidate> selectedCandidate;

  QDir drmDir("/sys/class/drm");
  const QStringList entries = drmDir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
  for (const QString& entry : entries) {
    if (!isDrmCardDirectoryName(entry)) {
      continue;
    }

    const QString cardPath = drmDir.filePath(entry);
    const QString vramTotalPath = drmDir.filePath(entry + "/device/mem_info_vram_total");
    const auto bytes = readUnsignedIntegerFile(vramTotalPath);
    if (!bytes.has_value() || *bytes == 0) {
      continue;
    }

    DrmMemoryCandidate candidate;
    candidate.cardName = entry;
    candidate.vramTotalPath = vramTotalPath;
    candidate.driverName = drmDriverName(cardPath);
    candidate.capacityBytes = *bytes;
    candidate.hasPciIdentity = hasPciDeviceIdentity(cardPath);
    candidate.hasRenderNode = hasRenderNode(cardPath);
    candidate.isVirtualOrSimple = isObviousVirtualOrSimpleDriver(candidate.driverName);

    VLOG(1) << "Detected GPU VRAM candidate " << candidate.cardName << " from " << candidate.vramTotalPath << ": "
            << candidate.capacityBytes << " bytes, driver=" << candidate.driverName
            << ", pci=" << candidate.hasPciIdentity << ", renderNode=" << candidate.hasRenderNode
            << ", virtualOrSimple=" << candidate.isVirtualOrSimple << ", rank=" << candidate.rank();

    if (!selectedCandidate.has_value() || isPreferredCandidate(candidate, *selectedCandidate)) {
      selectedCandidate = std::move(candidate);
    }
  }

  if (selectedCandidate.has_value()) {
    info.capacityBytes = selectedCandidate->capacityBytes;
    info.source = QStringLiteral("Linux DRM sysfs mem_info_vram_total (%1)").arg(selectedCandidate->cardName);
    info.hasUnifiedMemory = false;
    VLOG(1) << "Selected GPU VRAM candidate " << selectedCandidate->cardName << " from "
            << selectedCandidate->vramTotalPath << ": " << selectedCandidate->capacityBytes << " bytes";
  }

  return info;
}

} // namespace nim
