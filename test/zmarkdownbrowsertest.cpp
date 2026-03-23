#include "zmarkdownbrowser.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <memory>

namespace {

class ScopedQtWidgetsApplication
{
public:
  ScopedQtWidgetsApplication()
  {
    if (QApplication::instance() != nullptr) {
      return;
    }

#if defined(__linux__)
    static int argc = 3;
    static char arg0[] = "zmarkdownbrowsertest";
    static char arg1[] = "-platform";
    static char arg2[] = "offscreen";
    static char* argv[] = {arg0, arg1, arg2, nullptr};
    m_app = std::make_unique<QApplication>(argc, argv);
#else
    static int argc = 1;
    static char arg0[] = "zmarkdownbrowsertest";
    static char* argv[] = {arg0, nullptr};
    m_app = std::make_unique<QApplication>(argc, argv);
#endif
  }

private:
  std::unique_ptr<QApplication> m_app;
};

[[nodiscard]] QString writeMarkdownFile(const QString& dirPath, const QString& fileName, const QString& markdown)
{
  const QString path = QDir(dirPath).filePath(fileName);
  QFile file(path);
  EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
  EXPECT_EQ(file.write(markdown.toUtf8()), markdown.toUtf8().size());
  return path;
}

TEST(ZMarkdownBrowser, InjectsAnchorsForMarkdownHeadings)
{
  ScopedQtWidgetsApplication app;
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = writeMarkdownFile(tmp.path(),
                                         QStringLiteral("guide.md"),
                                         QStringLiteral("# Overview\n"
                                                        "\n"
                                                        "- [Shortcuts](#121-keyboard-and-mouse-shortcuts)\n"
                                                        "\n"
                                                        "## 12.1 Keyboard and Mouse Shortcuts\n"
                                                        "Atlas shortcut reference.\n"
                                                        "\n"
                                                        "## Duplicate Heading\n"
                                                        "One.\n"
                                                        "\n"
                                                        "## Duplicate Heading\n"
                                                        "Two.\n"));

  nim::ZMarkdownBrowser browser;
  browser.navigateTo(QUrl::fromLocalFile(path));

  const QString html = browser.document()->toHtml();
  EXPECT_TRUE(html.contains(QStringLiteral("name=\"overview\""))) << html.toStdString();
  EXPECT_TRUE(html.contains(QStringLiteral("name=\"121-keyboard-and-mouse-shortcuts\""))) << html.toStdString();
  EXPECT_TRUE(html.contains(QStringLiteral("name=\"duplicate-heading\""))) << html.toStdString();
  EXPECT_TRUE(html.contains(QStringLiteral("name=\"duplicate-heading-1\""))) << html.toStdString();
}

} // namespace
