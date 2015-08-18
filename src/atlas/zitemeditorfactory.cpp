#include "zitemeditorfactory.h"
#include <QDoubleSpinBox>

namespace nim {

ZItemEditorFactory::ZItemEditorFactory()
  : QItemEditorFactory()
{
}

#ifndef _QT4_
QWidget *ZItemEditorFactory::createEditor(int type, QWidget *parent) const
#else
QWidget *ZItemEditorFactory::createEditor(QVariant::Type type, QWidget *parent) const
#endif
{
  if (type == QVariant::Double) {
    QDoubleSpinBox *sb = new QDoubleSpinBox(parent);
    sb->setFrame(false);
    sb->setDecimals(10);
    sb->setSingleStep(1e-10);
    sb->setMinimum(-std::numeric_limits<double>::max());
    sb->setMaximum(std::numeric_limits<double>::max());
    return sb;
  } else {
    return QItemEditorFactory::createEditor(type, parent);
  }
}

} // namespace nim
