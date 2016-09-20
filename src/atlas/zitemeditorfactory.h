#pragma once

#include <QItemEditorFactory>

namespace nim {

class ZItemEditorFactory : public QItemEditorFactory
{
public:

  virtual QWidget* createEditor(int type, QWidget* parent) const override;

};

} // namespace nim

