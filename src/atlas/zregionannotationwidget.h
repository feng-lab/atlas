#pragma once

#include "zdoc.h"
#include "zregionannotationpack.h"
#include <QWidget>

class QPushButton;

namespace nim {

class ZRegionAnnotationWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ZRegionAnnotationWidget(ZRegionAnnotationPack& rap, ZDoc& doc, QWidget* parent = nullptr);

protected:
  void exportLabelImage();

  void transformMesh();

  void interpolateRegionAnnotation();

  void interpolateRegionAnnotation2();

  void updateMesh();

  void exportMeshes();

  void exportSvgImage();

private:
  void createWidget();

  void onLockedStateChanged(bool v);

private:
  ZRegionAnnotationPack& m_regionAnnotationPack;
  ZDoc& m_doc;

  QPushButton* m_interpolateRegionAnnotationButton = nullptr;
  QPushButton* m_interpolateRegionAnnotationButton2 = nullptr;
  QPushButton* m_update3DMeshFromROIButton = nullptr;
  QPushButton* m_transform3DMeshButton = nullptr;
  QPushButton* m_exportLableImageButton = nullptr;
  QPushButton* m_export3DMeshes = nullptr;
  QPushButton* m_exportSvgImageButton = nullptr;
};

} // namespace nim
