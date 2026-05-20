#include "zlogwidget.h"
#include "zlogcache.h"
#include "ztheme.h"
#include <vector>

namespace nim {

namespace {

[[nodiscard]] QColor logErrorTextColor()
{
  return ZTheme::instance().isDarkTheme() ? QColor(242, 182, 179) : QColor(176, 0, 32);
}

} // namespace

ZLogWidget::ZLogWidget(bool receiveOldMessages, QWidget* parent)
  : QPlainTextEdit(parent)
{
  // setCenterOnScroll(true);
  m_normalFormat = currentCharFormat();
  m_errorFormat = m_normalFormat;
  m_errorFormat.setForeground(QBrush(logErrorTextColor()));
  connect(&ZTheme::instance(), &ZTheme::themeChanged, this, [this]() {
    m_normalFormat = currentCharFormat();
    m_errorFormat = m_normalFormat;
    m_errorFormat.setForeground(QBrush(logErrorTextColor()));
  });
  ZLogCache::instance().receiveLogMessages(this, &ZLogWidget::writeLogData, receiveOldMessages);
}

void ZLogWidget::writeLogData(const std::vector<LogData>* messages, size_t start, size_t end)
{
  if (end - start == 1) {
    const auto& logData = (*messages)[start];
    if (logData.level <= absl::LogSeverity::kInfo) {
      appendPlainText(QString::fromStdString(logData.formatted));
    } else {
      setCurrentCharFormat(m_errorFormat);
      appendPlainText(QString::fromStdString(logData.formatted));
      setCurrentCharFormat(m_normalFormat);
    }
  } else {
    const auto& logData = (*messages)[start];
    bool firstFormat = logData.level <= absl::LogSeverity::kInfo;
    bool lastFormat = firstFormat;
    std::vector<QStringList> textList;
    textList.emplace_back();
    textList.back().push_back(QString::fromStdString(logData.formatted));
    for (auto i = start + 1; i < end; ++i) {
      const auto& logD = (*messages)[i];
      if ((logD.level <= absl::LogSeverity::kInfo) != lastFormat) {
        lastFormat = !lastFormat;
        textList.emplace_back();
      }
      textList.back().push_back(QString::fromStdString(logD.formatted));
    }
    for (const auto& si : textList) {
      setCurrentCharFormat(firstFormat ? m_normalFormat : m_errorFormat);
      firstFormat = !firstFormat;
      appendPlainText(si.join("\n"));
    }
    setCurrentCharFormat(m_normalFormat);
  }
}

} // namespace nim
