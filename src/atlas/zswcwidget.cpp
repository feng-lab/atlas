#include "zswcwidget.h"

#include "zswctreemodel.h"
#include "zswctreeview.h"
#include <QVBoxLayout>

namespace nim {

ZSwcWidget::ZSwcWidget(ZSwcPack& p, ZDoc& doc, QWidget* parent)
  : QWidget(parent)
  , m_swcPack(p)
  , m_doc(doc)
{
  createWidget();
}

void ZSwcWidget::createWidget()
{
  auto vlo = new QVBoxLayout;

  auto model = new ZSwcTreeModel(m_swcPack, this);
  auto view = new ZSwcTreeView(*model, m_swcPack, m_doc, this);
  vlo->addWidget(view);

  setLayout(vlo);
}

} // namespace nim

