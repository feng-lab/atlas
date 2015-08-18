#ifndef ZREGIONANNOTATIONWIDGET_H
#define ZREGIONANNOTATIONWIDGET_H

#include <QWidget>
#include "zregionannotation.h"
#include "zdoc.h"

namespace nim {

class ZRegionAnnotationWidget : public QWidget
{
  Q_OBJECT
public:
  explicit ZRegionAnnotationWidget(ZRegionAnnotation &anno, ZDoc &doc, QWidget *parent = 0);

signals:

public slots:

protected slots:
  void exportLabelImage();

protected:

private:
  void createWidget();

private:
  ZRegionAnnotation &m_regionAnnotation;
  ZDoc &m_doc;
};

} // namespace nim

#endif // ZREGIONANNOTATIONWIDGET_H
