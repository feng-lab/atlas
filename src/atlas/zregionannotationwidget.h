#pragma once

#include "zdoc.h"
#include "zregionannotation.h"
#include <QWidget>

namespace nim {

class ZRegionAnnotationWidget : public QWidget
{
Q_OBJECT
public:
  explicit ZRegionAnnotationWidget(ZRegionAnnotation& anno, ZDoc& doc, QWidget* parent = nullptr);

protected:
  void exportLabelImage();

private:
  void createWidget();

private:
  ZRegionAnnotation& m_regionAnnotation;
  ZDoc& m_doc;
};

} // namespace nim

