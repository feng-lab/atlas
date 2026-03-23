#include "zmarkdownbrowser.h"

#include <QDir>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextLength>
#include <QUrl>
#include <QTimer>

namespace {

[[nodiscard]] QString stripOptionalClosingHashes(const QString& headingText)
{
  QString text = headingText.trimmed();
  static const QRegularExpression trailingHashesRe(QStringLiteral(R"(\s+#+\s*$)"));
  text.remove(trailingHashesRe);
  return text.trimmed();
}

[[nodiscard]] QString slugifyMarkdownHeading(const QString& headingText, QHash<QString, int>* duplicateCount)
{
  QString baseSlug;
  baseSlug.reserve(headingText.size());

  bool lastWasDash = false;
  for (const QChar ch : headingText) {
    if (ch.isLetterOrNumber()) {
      baseSlug.append(ch.toCaseFolded());
      lastWasDash = false;
      continue;
    }

    if ((ch.isSpace() || ch == QChar::fromLatin1('-')) && !baseSlug.isEmpty() && !lastWasDash) {
      baseSlug.append(QChar::fromLatin1('-'));
      lastWasDash = true;
    }
  }

  while (baseSlug.endsWith(QChar::fromLatin1('-'))) {
    baseSlug.chop(1);
  }

  if (baseSlug.isEmpty()) {
    baseSlug = QStringLiteral("section");
  }

  if (duplicateCount == nullptr) {
    return baseSlug;
  }

  const int seenCount = duplicateCount->value(baseSlug, 0);
  duplicateCount->insert(baseSlug, seenCount + 1);
  if (seenCount == 0) {
    return baseSlug;
  }
  return QStringLiteral("%1-%2").arg(baseSlug).arg(seenCount);
}

[[nodiscard]] QStringList collectMarkdownHeadingSlugs(const QString& markdown)
{
  static const QRegularExpression headingRe(QStringLiteral(R"(^\s{0,3}(#{1,6})\s+(.+?)\s*$)"));

  QStringList slugs;
  bool inCodeFence = false;
  QHash<QString, int> duplicateCount;

  int pos = 0;
  while (pos <= markdown.size()) {
    const int nextNewline = markdown.indexOf('\n', pos);
    const int lineEnd = (nextNewline < 0) ? markdown.size() : nextNewline;
    const QString line = markdown.mid(pos, lineEnd - pos);
    const QString trimmed = line.trimmed();

    if (trimmed.startsWith(QStringLiteral("```")) || trimmed.startsWith(QStringLiteral("~~~"))) {
      inCodeFence = !inCodeFence;
    } else if (!inCodeFence) {
      const QRegularExpressionMatch match = headingRe.match(line);
      if (match.hasMatch()) {
        const QString headingText = stripOptionalClosingHashes(match.captured(2));
        slugs.push_back(slugifyMarkdownHeading(headingText, &duplicateCount));
      }
    }

    if (nextNewline < 0) {
      break;
    }
    pos = nextNewline + 1;
  }

  return slugs;
}

[[nodiscard]] QString injectHeadingAnchors(const QString& html, const QStringList& headingSlugs)
{
  static const QRegularExpression headingTagRe(QStringLiteral(R"(<h[1-6][^>]*>)"),
                                               QRegularExpression::CaseInsensitiveOption);

  QString output;
  output.reserve(html.size() + headingSlugs.size() * 32);

  int last = 0;
  int headingIndex = 0;
  auto it = headingTagRe.globalMatch(html);
  while (it.hasNext()) {
    const QRegularExpressionMatch match = it.next();
    output += html.mid(last, match.capturedStart() - last);
    output += match.captured(0);
    if (headingIndex < headingSlugs.size()) {
      const QString escapedSlug = headingSlugs.at(headingIndex).toHtmlEscaped();
      output += QStringLiteral("<a name=\"%1\" id=\"%1\"></a>").arg(escapedSlug);
    }
    last = match.capturedEnd();
    ++headingIndex;
  }

  output += html.mid(last);
  return output;
}

QUrl baseUrlForResource(const QTextBrowser& browser)
{
  QUrl base = browser.document()->baseUrl();
  if (base.isEmpty()) {
    return base;
  }
  return base.adjusted(QUrl::RemoveFragment);
}

QString resolveLocalPath(const QTextBrowser& browser, const QUrl& url)
{
  if (url.isLocalFile()) {
    return url.toLocalFile();
  }

  QUrl resolved = url;
  if (resolved.isRelative()) {
    const QUrl base = baseUrlForResource(browser);
    if (!base.isEmpty()) {
      resolved = base.resolved(resolved);
    }
  }
  if (resolved.isLocalFile()) {
    return resolved.toLocalFile();
  }

  if (!url.scheme().isEmpty()) {
    return {};
  }

  QString path = url.path();
  if (path.startsWith('/')) {
    path = path.mid(1);
  }
  if (path.isEmpty()) {
    return {};
  }

  const QUrl base = baseUrlForResource(browser);
  if (base.isLocalFile()) {
    const QFileInfo baseInfo(base.toLocalFile());
    if (baseInfo.exists()) {
      const QString candidate = QDir(baseInfo.dir()).filePath(path);
      if (QFileInfo::exists(candidate)) {
        return candidate;
      }
    }
  }

  for (const QString& searchPath : browser.searchPaths()) {
    const QString candidate = QDir(searchPath).filePath(path);
    if (QFileInfo::exists(candidate)) {
      return candidate;
    }
  }

  return {};
}

QVariant renderSvgToImage(const QString& path, qreal devicePixelRatio)
{
  QSvgRenderer renderer(path);
  if (!renderer.isValid()) {
    return {};
  }

  QSize size = renderer.defaultSize();
  if (size.isEmpty()) {
    size = renderer.viewBoxF().size().toSize();
  }
  if (size.isEmpty()) {
    return {};
  }

  if (devicePixelRatio <= 0.0) {
    devicePixelRatio = 1.0;
  }
  const QSize imageSize = (QSizeF(size) * devicePixelRatio).toSize();
  if (imageSize.isEmpty()) {
    return {};
  }

  QImage image(imageSize, QImage::Format_ARGB32_Premultiplied);
  image.setDevicePixelRatio(devicePixelRatio);
  image.fill(Qt::white);

  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);
  renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(size)));
  painter.end();
  return image;
}

bool isSvgUrl(const QUrl& url)
{
  QString path = url.path();
  if (path.isEmpty()) {
    path = url.toString();
  }
  return path.endsWith(".svg", Qt::CaseInsensitive);
}

void applyImageMaximumWidth(QTextDocument* document)
{
  if (document == nullptr) {
    return;
  }

  struct ImageUpdate
  {
    int position = 0;
    int length = 0;
    QTextImageFormat format;
  };

  std::vector<ImageUpdate> updates;
  for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
      const QTextFragment fragment = it.fragment();
      if (!fragment.isValid()) {
        continue;
      }

      const QTextCharFormat format = fragment.charFormat();
      if (!format.isImageFormat()) {
        continue;
      }

      QTextImageFormat imageFormat = format.toImageFormat();
      imageFormat.setMaximumWidth(QTextLength(QTextLength::PercentageLength, 100.0));
      updates.push_back(ImageUpdate{fragment.position(), fragment.length(), imageFormat});
    }
  }

  if (updates.empty()) {
    return;
  }

  QTextCursor cursor(document);
  cursor.beginEditBlock();
  for (const ImageUpdate& update : updates) {
    cursor.setPosition(update.position);
    cursor.setPosition(update.position + update.length, QTextCursor::KeepAnchor);
    cursor.mergeCharFormat(update.format);
  }
  cursor.endEditBlock();
}

void positionDocument(QTextBrowser& browser, const QString& fragment)
{
  if (!fragment.isEmpty()) {
    browser.scrollToAnchor(fragment);
  } else {
    browser.moveCursor(QTextCursor::Start);
    browser.ensureCursorVisible();
  }
}

void schedulePositionDocument(QTextBrowser& browser, const QString& fragment)
{
  QTimer::singleShot(0, &browser, [&browser, fragment]() {
    positionDocument(browser, fragment);
  });
}

} // namespace

namespace nim {

ZMarkdownBrowser::ZMarkdownBrowser(QWidget* parent)
  : QTextBrowser(parent)
{
  setOpenLinks(false);
  QObject::connect(this, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
    if (url.isRelative() || url.isLocalFile() || url.scheme().isEmpty()) {
      navigateTo(url);
      return;
    }
    QDesktopServices::openUrl(url);
  });
  updateHistorySignals();
}

QVariant ZMarkdownBrowser::loadResource(int type, const QUrl& name)
{
  if (type == QTextDocument::ImageResource && isSvgUrl(name)) {
    const QString localPath = resolveLocalPath(*this, name);
    if (!localPath.isEmpty()) {
      QVariant image = renderSvgToImage(localPath, devicePixelRatioF());
      if (image.isValid()) {
        return image;
      }
    }
  }

  return QTextBrowser::loadResource(type, name);
}

namespace {

struct MarkdownImageRef
{
  QString token;
  QString alt;
  QString src;
};

QString readUtf8File(const QString& path, QString* errorOut)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    if (errorOut) {
      *errorOut = file.errorString();
    }
    return {};
  }
  const QByteArray data = file.readAll();
  return QString::fromUtf8(data);
}

QString preprocessMarkdownForImages(const QString& markdown, std::vector<MarkdownImageRef>* imagesOut)
{
  const QRegularExpression imageRe(QStringLiteral(R"(!\[([^\]]*)\]\(([^)\s]+)(?:\s+"[^"]*")?\))"));

  QString output;
  output.reserve(markdown.size());

  bool inCodeFence = false;
  int imageIndex = 0;
  int pos = 0;

  while (pos <= markdown.size()) {
    const int nextNewline = markdown.indexOf('\n', pos);
    const int lineEnd = (nextNewline < 0) ? markdown.size() : nextNewline;
    const QString line = markdown.mid(pos, lineEnd - pos);
    const QString trimmed = line.trimmed();

    if (trimmed.startsWith("```")) {
      inCodeFence = !inCodeFence;
      output += line;
    } else if (inCodeFence) {
      output += line;
    } else {
      QString processed;
      processed.reserve(line.size());

      int last = 0;
      auto it = imageRe.globalMatch(line);
      while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        processed += line.mid(last, match.capturedStart() - last);
        const QString token = QStringLiteral("@@ATLAS_IMG_%1@@").arg(imageIndex++);
        if (imagesOut) {
          imagesOut->push_back(
            MarkdownImageRef { token, match.captured(1), match.captured(2).trimmed() });
        }
        processed += token;
        last = match.capturedEnd();
      }
      processed += line.mid(last);
      output += processed;
    }

    if (nextNewline < 0) {
      break;
    }
    output += '\n';
    pos = nextNewline + 1;
  }

  return output;
}

QString markdownToHtmlWithImages(const QString& markdown)
{
  std::vector<MarkdownImageRef> images;
  const QString preprocessed = preprocessMarkdownForImages(markdown, &images);
  const QStringList headingSlugs = collectMarkdownHeadingSlugs(markdown);

  QTextDocument doc;
  doc.setMarkdown(preprocessed, QTextDocument::MarkdownDialectGitHub);
  QString html = injectHeadingAnchors(doc.toHtml(), headingSlugs);

  for (const MarkdownImageRef& img : images) {
    QString src = img.src;
    if (src.startsWith('<') && src.endsWith('>') && src.size() >= 2) {
      src = src.mid(1, src.size() - 2);
    }
    const QString imgTag = QStringLiteral("<img src=\"%1\" alt=\"%2\" />")
                             .arg(src.toHtmlEscaped(), img.alt.toHtmlEscaped());
    html.replace(img.token, imgTag);
  }

  return html;
}

} // namespace

void ZMarkdownBrowser::navigateTo(const QUrl& url)
{
  QUrl resolved = url;
  const QString fragment = resolved.fragment();
  resolved = resolved.adjusted(QUrl::RemoveFragment);

  if (resolved.isRelative()) {
    if (resolved.path().isEmpty() && !fragment.isEmpty() && m_currentSource.isValid()) {
      resolved = m_currentSource;
    } else if (m_currentSource.isValid()) {
      resolved = m_currentSource.resolved(resolved);
    } else if (!searchPaths().isEmpty()) {
      QString path = resolved.path();
      if (path.startsWith('/')) {
        path = path.mid(1);
      }
      for (const QString& searchPath : searchPaths()) {
        const QString candidate = QDir(searchPath).filePath(path);
        if (QFileInfo::exists(candidate)) {
          resolved = QUrl::fromLocalFile(candidate);
          break;
        }
      }
    }
  }

  if (!fragment.isEmpty()) {
    resolved.setFragment(fragment);
  }

  navigateInternal(resolved, true);
}

void ZMarkdownBrowser::goBack()
{
  if (m_historyIndex <= 0) {
    return;
  }
  --m_historyIndex;
  renderUrl(m_history.at(static_cast<size_t>(m_historyIndex)));
  updateHistorySignals();
}

void ZMarkdownBrowser::goForward()
{
  if (m_historyIndex < 0) {
    return;
  }
  const int nextIndex = m_historyIndex + 1;
  if (nextIndex >= static_cast<int>(m_history.size())) {
    return;
  }
  m_historyIndex = nextIndex;
  renderUrl(m_history.at(static_cast<size_t>(m_historyIndex)));
  updateHistorySignals();
}

void ZMarkdownBrowser::navigateInternal(const QUrl& url, bool pushHistory)
{
  if (pushHistory) {
    if (m_historyIndex + 1 < static_cast<int>(m_history.size())) {
      m_history.resize(static_cast<size_t>(m_historyIndex + 1));
    }
    m_history.push_back(url);
    m_historyIndex = static_cast<int>(m_history.size()) - 1;
  }

  renderUrl(url);
  updateHistorySignals();
}

void ZMarkdownBrowser::renderUrl(const QUrl& url)
{
  const QUrl withoutFragment = url.adjusted(QUrl::RemoveFragment);
  const QString fragment = url.fragment();

  if (!withoutFragment.isLocalFile()) {
    setPlainText(tr("Unsupported URL: %1").arg(url.toString()));
    return;
  }

  const QString path = withoutFragment.toLocalFile();
  const QFileInfo info(path);
  if (!info.exists()) {
    setPlainText(tr("File not found: %1").arg(path));
    return;
  }

  m_currentSource = QUrl::fromLocalFile(info.absoluteFilePath());

  const QUrl baseDir = QUrl::fromLocalFile(info.dir().absolutePath() + '/');

  const QString suffix = info.suffix().toLower();
  if (suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown")) {
    QString error;
    const QString markdown = readUtf8File(info.absoluteFilePath(), &error);
    if (markdown.isEmpty() && !error.isEmpty()) {
      setPlainText(tr("Failed reading %1: %2").arg(path, error));
      return;
    }
    document()->setBaseUrl(baseDir);
    setHtml(markdownToHtmlWithImages(markdown));
    document()->setBaseUrl(baseDir);
    applyImageMaximumWidth(document());
    schedulePositionDocument(*this, fragment);
  } else {
    QTextBrowser::setSource(m_currentSource);
    schedulePositionDocument(*this, fragment);
  }
}

void ZMarkdownBrowser::updateHistorySignals()
{
  const bool canGoBack = (m_historyIndex > 0);
  const bool canGoForward =
    (m_historyIndex >= 0) && (m_historyIndex + 1 < static_cast<int>(m_history.size()));
  Q_EMIT backwardAvailable(canGoBack);
  Q_EMIT forwardAvailable(canGoForward);
}

} // namespace nim
