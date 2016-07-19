#ifndef ZITEMEDITORFACTORY_H
#define ZITEMEDITORFACTORY_H

#include <QItemEditorFactory>

namespace nim {

class ZItemEditorFactory : public QItemEditorFactory
{
public:
  explicit ZItemEditorFactory();

  virtual QWidget* createEditor(int type, QWidget *parent) const override;

};

} // namespace nim

#endif // ZITEMEDITORFACTORY_H
