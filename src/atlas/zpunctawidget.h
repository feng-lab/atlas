#pragma once

#include "zdoc.h"
#include "zpunctapack.h"
#include <QWidget>

namespace nim {

class ZPunctaWidget : public QWidget
{
Q_OBJECT
public:
  explicit ZPunctaWidget(ZPunctaPack& p, ZDoc& doc, QWidget* parent = nullptr);

private:
  void createWidget();

private:
  ZPunctaPack& m_puncta;
  ZDoc& m_doc;
};

} // namespace nim

