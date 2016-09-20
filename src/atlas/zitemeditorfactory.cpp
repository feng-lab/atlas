#include "zitemeditorfactory.h"

#include <QDoubleSpinBox>

namespace nim {

QWidget* ZItemEditorFactory::createEditor(int type, QWidget* parent) const
{
  if (type == QVariant::Double) {
    auto sb = new QDoubleSpinBox(parent);
    sb->setFrame(false);
    sb->setDecimals(10);
    sb->setSingleStep(1e-10);
    sb->setMinimum(std::numeric_limits<double>::lowest());
    sb->setMaximum(std::numeric_limits<double>::max());
    return sb;
  } else {
    return QItemEditorFactory::createEditor(type, parent);
  }
}

} // namespace nim
