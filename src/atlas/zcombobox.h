#pragma once

#include <QComboBox>

namespace nim {

// prevent combobox steal mouse wheel event in a scroll area

class ZComboBoxEventFilter : public QObject
{
public:
  ZComboBoxEventFilter(QObject* parent = 0);

protected:
  virtual bool eventFilter(QObject* obj, QEvent* event) override;
};

class ZComboBox : public QComboBox
{
Q_OBJECT
public:
  explicit ZComboBox(QWidget* parent = 0);

  virtual QSize sizeHint() const override;

  virtual QSize minimumSizeHint() const override;

  void addItemSlot(const QString& text);

  void removeItemSlot(const QString& text);

protected:
  virtual void focusInEvent(QFocusEvent* event) override;

  virtual void focusOutEvent(QFocusEvent* event) override;
};

} // namespace nim

