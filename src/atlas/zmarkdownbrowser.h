#pragma once

#include <QTextBrowser>
#include <QUrl>

#include <vector>

namespace nim {

class ZMarkdownBrowser : public QTextBrowser
{
  Q_OBJECT

public:
  explicit ZMarkdownBrowser(QWidget* parent = nullptr);

  void navigateTo(const QUrl& url);
  void goBack();
  void goForward();

protected:
  QVariant loadResource(int type, const QUrl& name) override;

private:
  void navigateInternal(const QUrl& url, bool pushHistory);
  void renderUrl(const QUrl& url);
  void updateHistorySignals();

  std::vector<QUrl> m_history;
  int m_historyIndex = -1;
  QUrl m_currentSource;
};

} // namespace nim
