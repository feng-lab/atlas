#pragma once

#include "zdoc.h"
#include "zswcpack.h"
#include <QWidget>

namespace nim {

class ZSwcWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ZSwcWidget(ZSwcPack& p, ZDoc& doc, QWidget* parent = nullptr);

private:
  void createWidget();

private:
  ZSwcPack& m_swcPack;
  ZDoc& m_doc;
};

} // namespace nim
