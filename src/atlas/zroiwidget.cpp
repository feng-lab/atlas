#include "zroiwidget.h"

#include <QPushButton>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QApplication>
#include <QMessageBox>

namespace nim {

ZROIWidget::ZROIWidget(ZROI& roi, ZDoc& doc, QWidget* parent)
  : QWidget(parent)
  , m_roi(roi)
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


