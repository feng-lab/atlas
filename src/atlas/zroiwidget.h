#pragma once

#include "zdoc.h"
#include "zroi.h"
#include <QWidget>

namespace nim {

class ZROIWidget : public QWidget
{
Q_OBJECT
public:
  explicit ZROIWidget(ZROI& roi, ZDoc& doc, QWidget* parent = nullptr);

protected:

private:
  void createWidget();

private:
  ZROI& m_roi;
  ZDoc& m_doc;
};

} // namespace nim

