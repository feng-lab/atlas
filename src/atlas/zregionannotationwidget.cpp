#include "zregionannotationwidget.h"

#include <QPushButton>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QApplication>
#include <QMessageBox>
#include "zimg.h"
#include "zsysteminfo.h"
#include "zregionannotationtreemodel.h"
#include "zregionannotationtreeview.h"

namespace nim {

ZRegionAnnotationWidget::ZRegionAnnotationWidget(ZRegionAnnotation& anno, ZDoc& doc, QWidget* parent)
  : QWidget(parent)
  , m_regionAnnotation(anno)
  , m_doc(doc)
{
  createWidget();
}

void ZRegionAnnotationWidget::exportLabelImage()
{
  QStringList filters;
  QList<FileFormat> formats;
  QList<Compression> comps;
  ZImg::getQtWriteNameFilter(filters, formats, comps);

  int fmtIdx = -1;
  QString fn;
  {
    QFileDialog dialog(QApplication::activeWindow());
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters(filters);
    for (int i = 0; i < formats.size(); ++i) {
      if (formats[i] == FileFormat::MetaImage) {
        dialog.selectNameFilter(filters[i]);
      }
    }
    dialog.setDirectory(ZSystemInfoInstance.lastOpenedObjPath("RegionAnnotation"));
    dialog.setWindowTitle(tr("Export Region Annotation As Label Image"));
    if (dialog.exec()) {
      fmtIdx = filters.indexOf(dialog.selectedNameFilter());
      fn = dialog.selectedFiles().at(0);
    }
    dialog.close();
  }
  QApplication::processEvents();
  if (fmtIdx >= 0 && !fn.isEmpty()) {
    try {
      m_regionAnnotation.exportLabelImage(fn, formats[fmtIdx], comps[fmtIdx]);
      ZSystemInfoInstance.addFileToRecentFileList(fn);
      ZSystemInfoInstance.setLastOpenedImagePath(fn);
    }
    catch (const ZException& e) {
      QMessageBox::critical(QApplication::activeWindow(), tr("Can not export label image"),
                            e.what());
    }
  }
}

void ZRegionAnnotationWidget::createWidget()
{
  QVBoxLayout* vlo = new QVBoxLayout;

  QPushButton* pb = new QPushButton("Update 3D Mesh");
  pb->setToolTip("Update 3D mesh with current region contours");
  pb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  vlo->addWidget(pb, 0, Qt::AlignLeft | Qt::AlignVCenter);
  connect(pb, &QPushButton::clicked, &m_regionAnnotation, &ZRegionAnnotation::updateMesh);

  pb = new QPushButton("Export Label Image...");
  pb->setToolTip("Export Region Annotation To Label Image");
  pb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  vlo->addWidget(pb, 0, Qt::AlignLeft | Qt::AlignVCenter);
  connect(pb, &QPushButton::clicked, this, &ZRegionAnnotationWidget::exportLabelImage);

  ZRegionAnnotationTreeModel* model = new ZRegionAnnotationTreeModel(m_regionAnnotation, this);
  ZRegionAnnotationTreeView* view = new ZRegionAnnotationTreeView(*model, m_regionAnnotation, m_doc, this);
  vlo->addWidget(view);

  setLayout(vlo);
}

} // namespace nim
