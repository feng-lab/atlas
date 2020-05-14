#include "zroiwidget.h"

#include <QPushButton>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QApplication>
#include <QMessageBox>

namespace nim {

ZROIWidget::ZROIWidget(ZROIPack& roiPack, ZDoc& doc, QWidget* parent)
  : QWidget(parent)
  , m_roiPack(roiPack)
  , m_doc(doc)
{
  createWidget();
}

void ZROIWidget::createWidget()
{
  auto vlo = new QVBoxLayout;

  setLayout(vlo);
}

} // namespace nim


