#pragma once

#include <QComboBox>

namespace nim {

// prevent combobox steal mouse wheel event in a scroll area

class ZComboBoxEventFilter : public QObject
{
public:
  explicit ZComboBoxEventFilter(QObject* parent = nullptr);

protected:
  bool eventFilter(QObject* obj, QEvent* event) override;
};

class ZComboBox : public QComboBox
{
  Q_OBJECT

public:
  explicit ZComboBox(QWidget* parent = nullptr);

  QSize sizeHint() const override;

  QSize minimumSizeHint() const override;

  void addItemSlot(const QString& text);

  void removeItemSlot(const QString& text);

protected:
  void focusInEvent(QFocusEvent* event) override;

  void focusOutEvent(QFocusEvent* event) override;
};

} // namespace nim
