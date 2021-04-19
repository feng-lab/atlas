#include "zregionannotationwidget.h"

#include "zimg.h"
#include "zsysteminfo.h"
#include "zregionannotationtreemodel.h"
#include "zregionannotationtreeview.h"
#include "z3dtransformparameter.h"
#include "zparametereditdialog.h"
#include "zmeshdoc.h"
#include "zselectfilewidget.h"
#include <QPushButton>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QApplication>
#include <QMessageBox>
#include <QInputDialog>

namespace nim {

ZRegionAnnotationWidget::ZRegionAnnotationWidget(ZRegionAnnotationPack& rap, ZDoc& doc, QWidget* parent)
  : QWidget(parent)
  , m_regionAnnotationPack(rap)
  , m_doc(doc)
{
  createWidget();
  onLockedStateChanged(m_regionAnnotationPack.isLocked());
  connect(&m_regionAnnotationPack, &ZRegionAnnotationPack::lockedStateChanged,
          this, &ZRegionAnnotationWidget::onLockedStateChanged);
}

void ZRegionAnnotationWidget::exportLabelImage()
{
  QStringList filters;
  std::vector<FileFormat> formats;
  std::vector<Compression> comps;
  ZImg::getQtWriteNameFilter(filters, formats, comps);

  index_t fmtIdx;
  QString fn;
  FileFormat fileFormat;
  ZVec3Parameter ratioPara("Scale", glm::vec3(1.f), glm::vec3(1e-5), glm::vec3(1e10));

  {
    QDialog dialog(QApplication::activeWindow());
    auto alllayout = new QVBoxLayout;
    auto outputImageWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile, "Output Label Image:",
                                                   filters.join(";;"),
                                                   ZSystemInfo::instance().lastOpenedObjPath("RegionAnnotation"));
    for (size_t i = 0; i < formats.size(); ++i) {
      if (formats[i] == FileFormat::HDF5Img) {
        outputImageWidget->setSelectedFilter(filters[i]);
        break;
      }
    }
    alllayout->addWidget(outputImageWidget);
    ratioPara.setNameForEachValue({"X Scale:", "Y Scale:", "Z Scale:"});
    ratioPara.setDecimal(6);
    ratioPara.setStyle("SPINBOX");
    alllayout->addWidget(ratioPara.createWidget());
    auto bbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bbox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(bbox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    alllayout->addWidget(bbox);
    delete dialog.layout();

    dialog.setLayout(alllayout);
    dialog.setWindowTitle(tr("Export Region Annotation As Label Image"));

    if (dialog.exec()) {
      fmtIdx = filters.indexOf(outputImageWidget->selectedFilter());
      fn = outputImageWidget->getSelectedSaveFile();
    } else {
      return;
    }
    fileFormat = fmtIdx >= 0 ? formats[fmtIdx] : FileFormat::Unknown;
  }

  if (!fn.isEmpty()) {
    try {
      ZImgWriteParameters paras;
      paras.compression = comps[fmtIdx];
      m_regionAnnotationPack.regionAnnotation().exportLabelImage(fn, fileFormat, paras, ratioPara.get().x,
                                                                 ratioPara.get().y, ratioPara.get().z);
      ZSystemInfo::instance().addFileToRecentFileList(fn);
      ZSystemInfo::instance().setLastOpenedImagePath(fn);
    }
    catch (const ZException& e) {
      QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(),
                            QString("Can not export label image:\n%1").arg(e.what()));
    }
  }
}

void ZRegionAnnotationWidget::transformMesh()
{
  Z3DTransformParameter para("Transform Mesh");
  ZParameterEditDialog dialog(para);
  if (dialog.exec()) {
    m_regionAnnotationPack.regionAnnotation().transformMesh(para.get());
  }
}

void ZRegionAnnotationWidget::interpolateRegionAnnotation()
{
  bool ok;
  auto value = m_regionAnnotationPack.regionAnnotation().getOptimizedScale();
  double d = QInputDialog::getDouble(this, tr("Scale ROI before Interpolating"),
                                     tr("Scale:"), value, 1e-5, 1e10, 6, &ok,
                                     Qt::WindowFlags(), 1);
  if (ok) {
    m_regionAnnotationPack.regionAnnotation().interpolateRegionAnnotation(d);
  }
}

void ZRegionAnnotationWidget::interpolateRegionAnnotation2()
{
  bool ok;
  auto value = m_regionAnnotationPack.regionAnnotation().getOptimizedScale();
  double d = QInputDialog::getDouble(this, tr("Scale ROI before Interpolating"),
                                     tr("Scale:"), value, 1e-5, 1e10, 6, &ok,
                                     Qt::WindowFlags(), 1);
  if (ok) {
    m_regionAnnotationPack.regionAnnotation().interpolateRegionAnnotation2(d);
  }
}

void ZRegionAnnotationWidget::updateMesh()
{
  ZVec3Parameter ratioPara("Scale", glm::vec3(1.f), glm::vec3(1e-5), glm::vec3(1e10));

  {
    QDialog dialog(QApplication::activeWindow());
    auto alllayout = new QVBoxLayout;
    ratioPara.setNameForEachValue({"X Scale:", "Y Scale:", "Z Scale:"});
    ratioPara.setDecimal(6);
    ratioPara.setStyle("SPINBOX");
    alllayout->addWidget(ratioPara.createWidget());
    auto bbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bbox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(bbox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    alllayout->addWidget(bbox);
    delete dialog.layout();

    dialog.setLayout(alllayout);
    dialog.setWindowTitle(tr("Updating Meshes From Annotations"));

    if (dialog.exec()) {
      m_regionAnnotationPack.regionAnnotation().updateMesh(ratioPara.get().x, ratioPara.get().y, ratioPara.get().z);
    }
  }
}

void ZRegionAnnotationWidget::exportMeshes()
{
  QString dir = QFileDialog::getExistingDirectory(this, tr("Choose Directory for Meshes"),
                                                  ZSystemInfo::instance().lastOpenedObjPath("RegionAnnotation"),
                                                  QFileDialog::ShowDirsOnly
                                                  | QFileDialog::DontResolveSymlinks);
  if (!dir.isEmpty()) {
    QDir outDir(dir);
    for (const auto& rgn : m_regionAnnotationPack.regionAnnotation().annotationTree()) {
      if (m_regionAnnotationPack.regionAnnotation().meshOfRegion(rgn.id)) {
        QString fn = outDir.filePath(m_regionAnnotationPack.regionAnnotation().nameOfRegion(rgn.id) + ".obj");
        try {
          m_regionAnnotationPack.regionAnnotation().meshOfRegion(rgn.id)->save(fn);

          ZSystemInfo::instance().addFileToRecentFileList(fn);
          m_doc.meshDoc().setLastOpenedObjPath(fn);
        }
        catch (const ZException& e) {
          QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(),
                                QString("Save Mesh Error:\n%1").arg(e.what()));
        }
      }
    }
  }
}

void ZRegionAnnotationWidget::createWidget()
{
  auto vlo = new QVBoxLayout;

  m_interpolateRegionAnnotationButton = new QPushButton("Interpolate Region Annotations");
  m_interpolateRegionAnnotationButton->setToolTip("Interpolate regions for slices without annotations");
  m_interpolateRegionAnnotationButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  vlo->addWidget(m_interpolateRegionAnnotationButton, 0, Qt::AlignLeft | Qt::AlignVCenter);
  connect(m_interpolateRegionAnnotationButton, &QPushButton::clicked, this, &ZRegionAnnotationWidget::interpolateRegionAnnotation);

  m_interpolateRegionAnnotationButton2 = new QPushButton("Interpolate Region Annotations2");
  m_interpolateRegionAnnotationButton2->setToolTip("Interpolate regions for slices without annotations");
  m_interpolateRegionAnnotationButton2->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  vlo->addWidget(m_interpolateRegionAnnotationButton2, 0, Qt::AlignLeft | Qt::AlignVCenter);
  connect(m_interpolateRegionAnnotationButton2, &QPushButton::clicked, this, &ZRegionAnnotationWidget::interpolateRegionAnnotation2);

  m_update3DMeshFromROIButton = new QPushButton("Update 3D Meshes From Modified ROIs");
  m_update3DMeshFromROIButton->setToolTip("Update 3D meshes with current region contours");
  m_update3DMeshFromROIButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  vlo->addWidget(m_update3DMeshFromROIButton, 0, Qt::AlignLeft | Qt::AlignVCenter);
  connect(m_update3DMeshFromROIButton, &QPushButton::clicked, this, &ZRegionAnnotationWidget::updateMesh);

  m_transform3DMeshButton = new QPushButton("Transform 3D Mesh...");
  m_transform3DMeshButton->setToolTip("Apply transformation to 3D mesh");
  m_transform3DMeshButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  vlo->addWidget(m_transform3DMeshButton, 0, Qt::AlignLeft | Qt::AlignVCenter);
  connect(m_transform3DMeshButton, &QPushButton::clicked, this, &ZRegionAnnotationWidget::transformMesh);

  m_exportLableImageButton = new QPushButton("Export Label Image...");
  m_exportLableImageButton->setToolTip("Export Region Annotation To Label Image");
  m_exportLableImageButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  vlo->addWidget(m_exportLableImageButton, 0, Qt::AlignLeft | Qt::AlignVCenter);
  connect(m_exportLableImageButton, &QPushButton::clicked, this, &ZRegionAnnotationWidget::exportLabelImage);

  m_export3DMeshes = new QPushButton("Export 3D Meshes...");
  m_export3DMeshes->setToolTip("Export Region Annotation To 3D Meshes");
  m_export3DMeshes->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  vlo->addWidget(m_export3DMeshes, 0, Qt::AlignLeft | Qt::AlignVCenter);
  connect(m_export3DMeshes, &QPushButton::clicked, this, &ZRegionAnnotationWidget::exportMeshes);

  auto model = new ZRegionAnnotationTreeModel(m_regionAnnotationPack, this);
  auto view = new ZRegionAnnotationTreeView(*model, m_regionAnnotationPack, m_doc, this);
  vlo->addWidget(view);

  setLayout(vlo);
}

void ZRegionAnnotationWidget::onLockedStateChanged(bool)
{
  m_update3DMeshFromROIButton->setEnabled(!m_regionAnnotationPack.isLocked());
  m_transform3DMeshButton->setEnabled(!m_regionAnnotationPack.isLocked());
}

} // namespace nim
