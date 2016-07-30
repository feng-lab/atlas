#ifndef ZFONTWIDGET_H
#define ZFONTWIDGET_H

#include <QWidget>
#include <QFont>

class QLineEdit;

class QToolButton;

namespace nim {

class ZFontWidget : public QWidget
{
Q_OBJECT
public:
  explicit ZFontWidget(const QFont& font, QWidget* parent = nullptr);

  // default is false
  void setLabelFollowFontSize(bool v);

  void setFont(const QFont& font);

signals:

  void fontChanged(QFont font);

protected:
  void chooseFont();

private:
  QFont m_font;
  QLineEdit* m_label;
  QToolButton* m_button;
  bool m_followFontSize;
};

} // namespace nim

#endif // ZFONTWIDGET_H
