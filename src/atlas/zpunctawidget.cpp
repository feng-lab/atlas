#include "zpunctawidget.h"

#include "zpunctatablemodel.h"
#include "zpunctatableview.h"
#include <QVBoxLayout>

namespace nim {

ZPunctaWidget::ZPunctaWidget(ZPunctaPack& p, ZDoc& doc, QWidget* parent)
  : QWidget(parent)
  , m_puncta(p)
  , m_doc(doc)
{
  createWidget();
}

void ZPunctaWidget::createWidget()
{
  auto vlo = new QVBoxLayout;

  auto model = new ZPunctaTableModel(m_puncta, this);
  auto view = new ZPunctaTableView(*model, m_puncta, m_doc, this);
  vlo->addWidget(view);

  setLayout(vlo);
}

} // namespace nim
