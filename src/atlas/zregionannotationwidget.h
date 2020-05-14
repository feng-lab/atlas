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

private:
  void createWidget();

  void onLockedStateChanged(bool v);

private:
  ZRegionAnnotationPack& m_regionAnnotationPack;
  ZDoc& m_doc;

  QPushButton* m_update3DMeshFromROIButton = nullptr;
  QPushButton* m_transform3DMeshButton = nullptr;
  QPushButton* m_exportLableImageButton = nullptr;
};

} // namespace nim

