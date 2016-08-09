#pragma once

#include <QWidget>
#include "zregionannotation.h"
#include "zdoc.h"

namespace nim {

class ZRegionAnnotationWidget : public QWidget
{
Q_OBJECT
public:
  explicit ZRegionAnnotationWidget(ZRegionAnnotation& anno, ZDoc& doc, QWidget* parent = 0);

protected:
  void exportLabelImage();

private:
  void createWidget();

private:
  ZRegionAnnotation& m_regionAnnotation;
  ZDoc& m_doc;
};

} // namespace nim

