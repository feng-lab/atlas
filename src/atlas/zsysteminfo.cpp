#include "zsysteminfo.h"

#include "zlog.h"
#include "zmainwindow.h"
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSettings>
#include <QFileInfo>
#include <QApplication>
#include <QDateTime>
#include <QOperatingSystemVersion>
#include <chrono>

#if !defined(Q_OS_WIN) && !defined(Q_OS_DARWIN)
#include <sys/utsname.h> // for uname
#endif

namespace nim {

ZSystemInfo& ZSystemInfo::instance()
{
  static ZSystemInfo ins;
  return ins;
}

ZSystemInfo::ZSystemInfo()
{
  detectOS();

  // shader path
  m_shaderPath = ":/Resources/shader";

  // font path
  m_fontPath = ":/Resources/fonts";
}

void ZSystemInfo::logOSInfo() const
{
  LOG(INFO) << "OS: " << m_osString;
  LOG(INFO) << "OS: " << QSysInfo::prettyProductName();
  LOG(INFO) << "Kernel: " << QSysInfo::kernelType() + " " + QSysInfo::kernelVersion();
  LOG(INFO) << "Build ABI: " << QSysInfo::buildAbi();
  // LOG(INFO) << "Build CPU: " << QSysInfo::buildCpuArchitecture();
  LOG(INFO) << "Current CPU: " << QSysInfo::currentCpuArchitecture();
  LOG(INFO) << "Machine Host Name: " << QSysInfo::machineHostName();
  // LOG(INFO) << "Product Type: " << QSysInfo::productType();
  // LOG(INFO) << "Product Version: " << QSysInfo::productVersion();

  // time
  LOG(INFO) << "system_clock res: "
            << 1e9 * std::chrono::system_clock::period::num / std::chrono::system_clock::period::den << " ns";
  LOG(INFO) << "system_clock is_steady = " << std::boolalpha << std::chrono::system_clock::is_steady;

  LOG(INFO) << "steady_clock res: "
            << 1e9 * std::chrono::steady_clock::period::num / std::chrono::steady_clock::period::den << " ns";
  LOG(INFO) << "steady_clock is_steady = " << std::boolalpha << std::chrono::steady_clock::is_steady;

  VLOG(1) << "high_resolution_clock res: "
          << 1e9 * std::chrono::high_resolution_clock::period::num / std::chrono::high_resolution_clock::period::den
          << " ns";
  VLOG(1) << "high_resolution_clock is_steady = " << std::boolalpha << std::chrono::high_resolution_clock::is_steady;

  VLOG(1) << fmt::format("system_clock now: {}", std::chrono::system_clock::now());
  // VLOG(1) << fmt::format("{:%Y%m%d %H:%M:%S}", std::chrono::system_clock::now());
}

QString ZSystemInfo::shaderPath(const QString& filename) const
{
  return m_shaderPath + (filename.isEmpty() ? QString("") : QString("/") + filename);
}

QString ZSystemInfo::fontPath(const QString& filename) const
{
  return m_fontPath + (filename.isEmpty() ? QString("") : QString("/") + filename);
}

QDir ZSystemInfo::resourcesDir()
{
#ifdef Q_OS_MACOS
  return QDir(QCoreApplication::applicationDirPath() + u"/../Resources");
#else
  return QDir(QCoreApplication::applicationDirPath() + u"/Resources");
#endif
}

QString ZSystemInfo::resourcesDirPath()
{
  return resourcesDir().absolutePath();
}

QString ZSystemInfo::jreDirPath()
{
  return resourcesDir().absoluteFilePath("jre");
}

QString ZSystemInfo::jreArmDirPath()
{
  return resourcesDir().absoluteFilePath("jre-arm");
}

QString ZSystemInfo::jarsDirPath()
{
  return resourcesDir().absoluteFilePath("jars");
}

QString ZSystemInfo::applicationInstallDirPath()
{
#ifdef Q_OS_MACOS
  return QCoreApplication::applicationDirPath() + u"/../../..";
#elif defined(Q_OS_WIN64)
  return QCoreApplication::applicationDirPath() + u"/..";
#else
  return QCoreApplication::applicationDirPath() + u"/..";
#endif
}

QString ZSystemInfo::jsonDirPath()
{
  return resourcesDir().absoluteFilePath("json");
}

QDir ZSystemInfo::jsonDir()
{
  return QDir(jsonDirPath());
}

QString ZSystemInfo::imgCachePath(size_t requiredSpaceInBytes)
{
  auto tryPath = [&](QString basePath) -> QString {
    basePath = basePath.trimmed();
    if (basePath.isEmpty()) {
      return {};
    }

    QDir dir(basePath);
    if (!dir.exists() && !dir.mkpath(".")) {
      VLOG(1) << "imgCachePath candidate rejected: cannot create dir '" << basePath << "'";
      return {};
    }

    // On macOS, QStorageInfo may report the "/" system volume as read-only even for writable firmlink-backed paths
    // under /Users. Prefer an explicit directory write check over the storage-level read-only bit.
    const QFileInfo dirInfo(dir.absolutePath());
    if (!dirInfo.isDir() || !dirInfo.isReadable() || !dirInfo.isWritable()) {
      VLOG(1) << "imgCachePath candidate rejected: not writable '" << basePath << "'";
      return {};
    }

    QStorageInfo volumeInfo(basePath);
    const qint64 bytesAvailable = volumeInfo.bytesAvailable();
    VLOG(1) << basePath << " bytesAvailable=" << bytesAvailable << " valid=" << volumeInfo.isValid()
            << " ready=" << volumeInfo.isReady() << " readOnly=" << volumeInfo.isReadOnly()
            << " required=" << requiredSpaceInBytes;

    if (!volumeInfo.isValid() || !volumeInfo.isReady()) {
      return {};
    }
    if (bytesAvailable < 0) {
      // bytesAvailable() is qint64; negative means "unknown" (platform/filesystem-dependent). Avoid casting a negative
      // value to size_t (wraparound) which could incorrectly pass the required-space check.
      // Accept only when the caller doesn't require a specific amount.
      return (requiredSpaceInBytes == 0) ? basePath : QString{};
    }
    if (requiredSpaceInBytes > static_cast<size_t>(bytesAvailable)) {
      return {};
    }

    return basePath;
  };

  // Prefer app-local data location (stable across launches; not subject to OS cache eviction).
  const QString appLocal = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  QString folder = tryPath(appLocal);
  if (!folder.isEmpty()) {
    // For AppLocalDataLocation, return the directory directly. This is a stable, app-owned location where the user
    // expects Atlas to store persistent data, so an extra ".atlasImgCache" subdir is unnecessary.
    return folder;
  }

  // try other volumes
  if (folder.isEmpty()) {
    auto vols = QStorageInfo::mountedVolumes();
    for (auto& vol : vols) {
      VLOG(1) << "volume bytesAvailable=" << vol.bytesAvailable() << " rootPath=" << vol.rootPath()
              << " valid=" << vol.isValid() << " ready=" << vol.isReady() << " readOnly=" << vol.isReadOnly()
              << " isRoot=" << vol.isRoot();
      if (vol.isRoot() || !vol.isValid() || !vol.isReady()) {
        continue;
      }

      const QString candidate = tryPath(vol.rootPath());
      if (!candidate.isEmpty()) {
        folder = candidate;
        break;
      }
    }
  }

  if (!folder.isEmpty()) {
    const QString cacheDirName = QStringLiteral(".atlasImgCache");
    if (QDir(folder).mkpath(cacheDirName)) {
      return QDir(folder).filePath(cacheDirName);
    }
  }
  folder.clear();
  return folder;
}

QDir ZSystemInfo::logDir()
{
  static QDir dir = createLogDir();
  return dir;
}

QDir ZSystemInfo::configDir()
{
  static QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
  if (!dir.exists()) {
    dir.mkpath(".");
  }
  return dir;
}

QString ZSystemInfo::lastOpenedObjPath(const QString& typeName) const
{
  QSettings settings;
  QString res = settings.value(lastOpenedObjPathQSettingLocation(typeName)).toString();
  if (res.isEmpty()) {
#ifdef _WIN32
    res = "C:/";
#else
    res = "/";
#endif
  }
  return res;
}

void ZSystemInfo::setLastOpenedObjPath(const QString& typeName, const QString& path) const
{
  if (path.isEmpty()) {
    return;
  }
  auto fi = QFileInfo(path);
  QSettings settings;
  settings.setValue(lastOpenedObjPathQSettingLocation(typeName), fi.canonicalPath());
}

void ZSystemInfo::addFileToRecentFileList(const QString& fileName) const
{
  QSettings settings;
  QStringList files = settings.value("recentFileList").toStringList();
  QString fn = QFileInfo(fileName).canonicalFilePath();
  if (fn.isEmpty()) {
    const QString trimmed = fileName.trimmed();
    const bool isNetworkUrl =
      trimmed.startsWith("precomputed://", Qt::CaseInsensitive) || trimmed.startsWith("gs://", Qt::CaseInsensitive) ||
      trimmed.startsWith("s3://", Qt::CaseInsensitive) || trimmed.startsWith("http://", Qt::CaseInsensitive) ||
      trimmed.startsWith("https://", Qt::CaseInsensitive);
    if (!isNetworkUrl) {
      return;
    }
    fn = trimmed;
  }
  files.removeAll(fn);
  files.prepend(fn);
  while (files.size() > maxNumRecentFiles()) {
    files.removeLast();
  }

  settings.setValue("recentFileList", files);
  updateRecentFiles();
}

void ZSystemInfo::removeOldLogs(int numberToKeep)
{
  QDir ld = logDir();
  ld.cdUp();
  QStringList filters;
  filters << "????????"
             "-??????.???_LOG";
  QFileInfoList list = ld.entryInfoList(filters, QDir::Dirs | QDir::NoSymLinks, QDir::Name);
  for (int i = 0; i < list.size() - numberToKeep; ++i) {
    QDir logDir(list.at(i).absoluteFilePath());
    logDir.removeRecursively();
  }
}

void ZSystemInfo::updateRecentFiles()
{
  for (auto widget : QApplication::topLevelWidgets()) {
    if (auto mainWin = qobject_cast<ZMainWindow*>(widget)) {
      mainWin->updateRecentFileActions();
    }
  }
}

void ZSystemInfo::detectOS()
{
#if defined(Q_OS_DARWIN) || defined(Q_OS_WIN)
  auto current = QOperatingSystemVersion::current();
  m_osString = QString("%1 %2.%3.%4")
                 .arg(current.name())
                 .arg(current.majorVersion())
                 .arg(current.minorVersion())
                 .arg(current.microVersion());
#else
  utsname name;
  if (uname(&name) != 0) {
    return; // command not successful
  }

  m_osString = QString("%1 %2 %3 %4").arg(name.sysname).arg(name.release).arg(name.version).arg(name.machine);

#endif // Q_OS_WIN
}

QDir ZSystemInfo::createLogDir()
{
  QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
  if (!dir.exists()) {
    dir.mkpath(".");
  }
  QString logFolderName = "Logs";
#ifdef __APPLE__
  QDir osxLogDir = dir;
  // from ~/Library/Application Support/company/app to ~/Library/Logs
  if (osxLogDir.cdUp() && osxLogDir.cdUp() && osxLogDir.cdUp() && osxLogDir.cd("Logs")) {
    dir = osxLogDir;
    logFolderName = QCoreApplication::applicationName();
  }
#endif
  logFolderName += QString("/%1_LOG").arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-hhmmss.zzz"));
  if (!dir.mkpath(logFolderName)) {
    dir.remove(logFolderName);
  }
  if (dir.mkpath(logFolderName)) {
    return QDir(dir.absoluteFilePath(logFolderName));
  }

  return dir;
}

} // namespace nim
