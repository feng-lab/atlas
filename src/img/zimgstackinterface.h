#pragma once

#ifdef _NEUTUBE_

#include "zstack.hxx"
#include <string>
#include "zimginterface.h"

namespace nim {

class ZImg;

// convert img to zstack, img will lose all its data and be destroyed
ZStack* imgToZStack(ZImg& img, ZStack *data = nullptr);

// create a virtual zimg from stack
ZImg wrapZStackAsZImg(const ZStack& stack);

ZStack* readZStack(const std::string &filename, ZStack *data = nullptr, QString *error = nullptr);
bool writeZStack(const std::string &filename, const ZStack& stack, QString *error = nullptr);

ZStack* readZStack(const QStringList &fileList, Dimension catDim = Dimension::Z, QString *error = nullptr);

} // namespace

#endif // STACK_INTERFACE

