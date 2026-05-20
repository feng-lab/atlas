#include "zlogwidget.h"
#include "zlogcache.h"
#include "zlog.h"
#include "ztheme.h"
#include <QApplication>
#include <QFontDatabase>
#include <tuple>
#include <vector>

namespace {

constexpr qreal kLogFontPointSizeReduction = 1.0;
constexpr int kLogFontPixelSizeReduction = 1;

QFont fixedPitchLogFont()
{
  QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  const QFont uiFont = QApplication::font();
  if (uiFont.pointSizeF() > kLogFontPointSizeReduction) {
    font.setPointSizeF(uiFont.pointSizeF() - kLogFontPointSizeReduction);
  } else if (uiFont.pointSizeF() > 0) {
    font.setPointSizeF(uiFont.pointSizeF());
  } else if (uiFont.pixelSize() > kLogFontPixelSizeReduction) {
    font.setPixelSize(uiFont.pixelSize() - kLogFontPixelSizeReduction);
  } else if (uiFont.pixelSize() > 0) {
    font.setPixelSize(uiFont.pixelSize());
  }
  font.setStyleHint(QFont::Monospace);
  font.setFixedPitch(true);
  return font;
}

} // namespace

namespace nim {

namespace {

[[nodiscard]] QTextCharFormat logFormat(const QTextCharFormat& baseFormat, ZTheme::Color role)
{
  QTextCharFormat format = baseFormat;
  const QColor color = ZTheme::instance().color(role);
  CHECK(color.isValid()) << "ZTheme returned an invalid log color for role " << role;
  format.setForeground(QBrush(color));
  return format;
}

} // namespace

ZLogWidget::ZLogWidget(bool receiveOldMessages, QWidget* parent)
  : QPlainTextEdit(parent)
{
  // setCenterOnScroll(true);
  setFont(fixedPitchLogFont());
  updateFormats();
  connect(&ZTheme::instance(), &ZTheme::themeChanged, this, [this]() {
    updateFormats();
  });
  ZLogCache::instance().receiveLogMessages(this, &ZLogWidget::writeLogData, receiveOldMessages);
}

void ZLogWidget::updateFormats()
{
  const QTextCharFormat baseFormat;
  m_normalFormat = logFormat(baseFormat, ZTheme::LogNormalMessageTextColor);
  m_warningFormat = logFormat(baseFormat, ZTheme::LogWarningMessageTextColor);
  m_errorFormat = logFormat(baseFormat, ZTheme::LogErrorMessageTextColor);
  setCurrentCharFormat(m_normalFormat);
}

void ZLogWidget::writeLogData(const std::vector<LogData>* messages, size_t start, size_t end)
{
  auto isNormalLogLevel = [](absl::LogSeverity level) {
    return level <= absl::LogSeverity::kInfo;
  };

  auto formatForLogLevel = [this](absl::LogSeverity level) -> const QTextCharFormat& {
    if (level <= absl::LogSeverity::kInfo) {
      return m_normalFormat;
    }
    if (level == absl::LogSeverity::kWarning) {
      return m_warningFormat;
    }
    return m_errorFormat;
  };

  if (end - start == 1) {
    const auto& logData = (*messages)[start];
    if (!isNormalLogLevel(logData.level)) {
      setCurrentCharFormat(formatForLogLevel(logData.level));
    }
    appendPlainText(QString::fromStdString(logData.formatted));
    if (!isNormalLogLevel(logData.level)) {
      setCurrentCharFormat(m_normalFormat);
    }
  } else {
    const auto& logData = (*messages)[start];
    std::vector<std::tuple<absl::LogSeverity, const QTextCharFormat*, QStringList>> textList;
    textList.emplace_back(logData.level, &formatForLogLevel(logData.level), QStringList{});
    std::get<2>(textList.back()).push_back(QString::fromStdString(logData.formatted));
    for (auto i = start + 1; i < end; ++i) {
      const auto& logD = (*messages)[i];
      const QTextCharFormat* format = &formatForLogLevel(logD.level);
      if (format != std::get<1>(textList.back())) {
        textList.emplace_back(logD.level, format, QStringList{});
      }
      std::get<2>(textList.back()).push_back(QString::fromStdString(logD.formatted));
    }
    for (const auto& [level, format, lines] : textList) {
      if (!isNormalLogLevel(level)) {
        setCurrentCharFormat(*format);
      }
      appendPlainText(lines.join("\n"));
      if (!isNormalLogLevel(level)) {
        setCurrentCharFormat(m_normalFormat);
      }
    }
  }
}

} // namespace nim
