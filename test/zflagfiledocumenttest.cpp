#include "zflagfiledocument.h"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace nim {
namespace {

[[nodiscard]] QString writeTextFile(const QString& dirPath, const QString& fileName, const QString& text)
{
  const QString path = QDir(dirPath).filePath(fileName);
  QFile file(path);
  EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  EXPECT_EQ(file.write(text.toUtf8()), text.toUtf8().size());
  return path;
}

[[nodiscard]] QString readTextFile(const QString& path)
{
  QFile file(path);
  EXPECT_TRUE(file.open(QIODevice::ReadOnly));
  return QString::fromUtf8(file.readAll());
}

TEST(ZFlagfileDocument, LoadsManagedValuesDuplicatesAndPreservedManualBlock)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = writeTextFile(tmp.path(),
                                     QStringLiteral("user_settings_flagfile.txt"),
                                     QStringLiteral("# Direct edit outside the preserved block.\n"
                                                    "--atlas_http_backend=proxygen\n"
                                                    "--atlas_http_backend=curl\n"
                                                    "\n"
                                                    "# ---- Atlas preserved manual entries: begin ----\n"
                                                    "# Custom settings kept across GUI saves\n"
                                                    "--atlas_custom_debug_option=1\n"
                                                    "--atlas_http_ca_bundle=/tmp/certs.pem\n"
                                                    "# ---- Atlas preserved manual entries: end ----\n"));

  ZFlagfileDocument document;
  QString error;
  ASSERT_TRUE(document.load(path, atlasManagedFlagNames(), &error)) << error.toStdString();

  EXPECT_TRUE(document.fileExistedAtLoad());
  EXPECT_TRUE(document.hasManagedValue(QStringLiteral("atlas_http_backend")));
  EXPECT_EQ(document.managedValue(QStringLiteral("atlas_http_backend")), QStringLiteral("curl"));
  EXPECT_TRUE(document.hasManagedValue(QStringLiteral("atlas_http_ca_bundle")));
  EXPECT_EQ(document.managedValue(QStringLiteral("atlas_http_ca_bundle")), QStringLiteral("/tmp/certs.pem"));

  const QStringList duplicates = document.duplicateManagedFlags();
  ASSERT_EQ(duplicates.size(), 1);
  EXPECT_EQ(duplicates.front(), QStringLiteral("atlas_http_backend"));

  const QStringList preserved = document.preservedManualLines();
  ASSERT_EQ(preserved.size(), 2);
  EXPECT_EQ(preserved.at(0), QStringLiteral("# Custom settings kept across GUI saves"));
  EXPECT_EQ(preserved.at(1), QStringLiteral("--atlas_custom_debug_option=1"));

  EXPECT_TRUE(document.matchesFileOnDisk(path, &error)) << error.toStdString();
}

TEST(ZFlagfileDocument, WriteFileRendersManagedSectionAndRoundTripsManualBlock)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = QDir(tmp.path()).filePath(QStringLiteral("user_settings_flagfile.txt"));

  std::vector<ZManagedFlagfileEntry> entries;
  entries.push_back({QStringLiteral("Rendering"),
                     QStringLiteral("Default 3D render backend"),
                     QStringLiteral("atlas_default_render_backend"),
                     QStringLiteral("Choose the default backend for 3D windows."),
                     QStringLiteral("vulkan")});
  entries.push_back({QStringLiteral("Logging & Debugging"),
                     QStringLiteral("Global log verbosity"),
                     QStringLiteral("v"),
                     QStringLiteral("Increase for more verbose startup and runtime logs."),
                     QStringLiteral("2")});

  const QStringList preservedManualLines = {QStringLiteral("# Keep custom flags here"),
                                            QStringLiteral("--atlas_extra_custom_toggle=true")};

  QString error;
  ASSERT_TRUE(ZFlagfileDocument::writeFile(path, entries, preservedManualLines, &error)) << error.toStdString();

  const QString text = readTextFile(path);
  EXPECT_TRUE(text.contains(QStringLiteral("# Atlas user settings")));
  EXPECT_TRUE(text.contains(QStringLiteral("# Rendering")));
  EXPECT_TRUE(text.contains(QStringLiteral("--atlas_default_render_backend=vulkan")));
  EXPECT_TRUE(text.contains(QStringLiteral("--v=2")));
  EXPECT_TRUE(text.contains(QStringLiteral("# ---- Atlas preserved manual entries: begin ----")));
  EXPECT_TRUE(text.contains(QStringLiteral("--atlas_extra_custom_toggle=true")));
  EXPECT_TRUE(text.contains(QStringLiteral("# ---- Atlas preserved manual entries: end ----")));

  ZFlagfileDocument document;
  ASSERT_TRUE(document.load(path, atlasManagedFlagNames(), &error)) << error.toStdString();
  EXPECT_EQ(document.managedValue(QStringLiteral("atlas_default_render_backend")), QStringLiteral("vulkan"));
  EXPECT_EQ(document.managedValue(QStringLiteral("v")), QStringLiteral("2"));
  EXPECT_EQ(document.preservedManualLines(), preservedManualLines);
}

TEST(ZFlagfileDocument, DetectsExternalChangesAfterLoad)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = writeTextFile(tmp.path(),
                                     QStringLiteral("user_settings_flagfile.txt"),
                                     QStringLiteral("--atlas_default_render_backend=opengl\n"));

  ZFlagfileDocument document;
  QString error;
  ASSERT_TRUE(document.load(path, atlasManagedFlagNames(), &error)) << error.toStdString();

  const QString rewrittenPath = writeTextFile(tmp.path(),
                                              QStringLiteral("user_settings_flagfile.txt"),
                                              QStringLiteral("--atlas_default_render_backend=vulkan\n"));
  EXPECT_EQ(rewrittenPath, path);

  EXPECT_FALSE(document.matchesFileOnDisk(path, &error));
  EXPECT_FALSE(error.isEmpty());
}

TEST(ZFlagSettingsRegistry, ExposesOnlyAvailableFlagsForThisBuild)
{
  EXPECT_EQ(atlasFindFlagSettingSpec(QStringLiteral("zimg_use_fftw_for_fft_if_available")), nullptr);
  EXPECT_FALSE(atlasManagedFlagNames().contains(QStringLiteral("zimg_use_fftw_for_fft_if_available")));

  gflags::CommandLineFlagInfo info;
  for (const auto& spec : atlasFlagSettingSpecs()) {
    const QByteArray flagName = spec.name.toUtf8();
    EXPECT_TRUE(gflags::GetCommandLineFlagInfo(flagName.constData(), &info)) << flagName.constData();
  }

  const bool hasLlfioFlag = gflags::GetCommandLineFlagInfo("zimg_llfio_mapped_file_handle_flags", &info);
  EXPECT_EQ(atlasManagedFlagNames().contains(QStringLiteral("zimg_llfio_mapped_file_handle_flags")), hasLlfioFlag);
}

} // namespace
} // namespace nim
