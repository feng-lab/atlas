#include "zlogwidget.h"
#include "zlogcache.h"

namespace nim {

ZLogWidget::ZLogWidget(bool receiveOldMessages, QWidget* parent)
  : QPlainTextEdit(parent)
{
  //setCenterOnScroll(true);
  m_normalFormat = currentCharFormat();
  m_errorFormat = m_normalFormat;
  m_errorFormat.setForeground(QBrush(QColor(176, 0, 0)));
  ZLogCache::instance().receiveLogMessages(this, &ZLogWidget::writeLogData, receiveOldMessages);
}

void ZLogWidget::writeLogData(const QList<nim::LogData>* messages, int start, int end)
{
  if (end - start == 1) {
    if (messages->at(start).level <= InfoLevel) {
      appendPlainText(messages->at(start).formatted);
    } else {
      setCurrentCharFormat(m_errorFormat);
      appendPlainText(messages->at(start).formatted);
      setCurrentCharFormat(m_normalFormat);
    }
  } else {
    bool firstFormat = messages->at(start).level <= InfoLevel;
    bool lastFormat = firstFormat;
    QList<QStringList> textList;
    textList.push_back(QStringList());
    textList.back().push_back(messages->at(start).formatted);
    for (int i = start + 1; i < end; ++i) {
      if ((messages->at(i).level <= InfoLevel) != lastFormat) {
        lastFormat = !lastFormat;
        textList.push_back(QStringList());
      }
      textList.back().push_back(messages->at(i).formatted);
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

