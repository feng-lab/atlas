#ifndef ZITEMEDITORFACTORY_H
#define ZITEMEDITORFACTORY_H

#include <QItemEditorFactory>

namespace nim {

class ZItemEditorFactory : public QItemEditorFactory
{
public:
  explicit ZItemEditorFactory();

#ifndef _QT4_
  virtual QWidget* createEditor(int type, QWidget *parent) const override;
#else
  virtual QWidget* createEditor(QVariant::Type type, QWidget *parent) const override;
#endif

};

} // namespace nim

#endif // ZITEMEDITORFACTORY_H
