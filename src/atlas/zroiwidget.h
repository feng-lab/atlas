#pragma once

#include "zdoc.h"
#include "zroipack.h"
#include <QWidget>

namespace nim {

class ZROIWidget : public QWidget
{
Q_OBJECT
public:
  explicit ZROIWidget(ZROIPack& roiPack, ZDoc& doc, QWidget* parent = nullptr);

protected:

private:
  void createWidget();

private:
  ZROIPack& m_roiPack;
  ZDoc& m_doc;
};

} // namespace nim

