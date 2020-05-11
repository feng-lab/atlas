#pragma once

#include "zdoc.h"
#include "zpuncta.h"
#include <QWidget>

namespace nim {

class ZPunctaWidget : public QWidget
{
Q_OBJECT
public:
  explicit ZPunctaWidget(ZPuncta& p, ZDoc& doc, QWidget* parent = nullptr);

private:
  void createWidget();

private:
  ZPuncta& m_puncta;
  ZDoc& m_doc;
};

} // namespace nim

