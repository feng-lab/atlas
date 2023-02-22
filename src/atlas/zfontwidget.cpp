#include "zfontwidget.h"

#include <QLineEdit>
#include <QFontDialog>
#include <QToolButton>
#include <QHBoxLayout>

namespace nim {

ZFontWidget::ZFontWidget(const QFont& font, QWidget* parent)
  : QWidget(parent)
  , m_font(font)
  , m_followFontSize(false)
{
  m_label = new QLineEdit;
  m_label->setReadOnly(true);
  m_label->setText(m_font.key());
  setLabelFollowFontSize(m_followFontSize);
  m_button = new QToolButton();
  m_button->setText("...");
  connect(m_button, &QToolButton::clicked, this, &ZFontWidget::chooseFont);
  auto lo = new QHBoxLayout(this);
  lo->setContentsMargins(0, 0, 0, 0);
  lo->addWidget(m_label);
  lo->addWidget(m_button);
}

void ZFontWidget::setLabelFollowFontSize(bool v)
{
  m_followFontSize = v;
  if (m_followFontSize) {
    m_label->setFont(m_font);
  } else {
    QFont cpy = m_font;
    cpy.setPointSize(QFont().pointSize());
    m_label->setFont(cpy);
  }
  m_label->home(false);
}

void ZFontWidget::set_Font(const QFont& font)
{
  if (font != m_font) {
    m_font = font;
    m_label->setText(m_font.key());
    setLabelFollowFontSize(m_followFontSize);
    Q_EMIT fontChanged(m_font);
  }
}

void ZFontWidget::chooseFont()
{
  bool ok;
  QFont font = QFontDialog::getFont(&ok, m_font, this, "Select Font");
  if (ok) {
    set_Font(font);
  }
}

} // namespace nim
