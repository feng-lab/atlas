#ifndef ZQTHEADER_H
#define ZQTHEADER_H

#ifndef _QT_GUI_USED_

class ZQImage {};
#define QImage ZQImage

class ZQPainter{};
#define QPainter ZQPainter

class ZQColor {};
#define QColor ZQColor

//class ZQPoint {
//public:
//  ZQPoint() {}
//  ZQPoint(int, int) {}
//};
//#define QPoint ZQPoint
//
//class ZQPointF {
//public:
//  ZQPointF() {}
//  ZQPointF(double, double) {}
//};
//#define QPointF ZQPointF

class ZQPaintDevice{};
#define QPaintDevice ZQPaintDevice

//class ZQRectF{};
//#define QRectF ZQRectF
//
//class ZQRect{};
//#define QRect ZQRect

#else
#include <QObject>
#include <QPainter>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QPaintDevice>
#include <QString>
#include <QList>
#include <QRectF>
#include <QRect>
#include <QDebug>
#include <QPen>
#include <QColor>
#include "qt/gui/utilities.h"
#endif

#endif // ZQTHEADER_H
