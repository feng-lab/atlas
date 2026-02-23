#pragma once

#include "zimginterface.h"
#include "zstack.hxx"

#include <QStringList>

#include <string>

namespace nim {

class ZImg;

// Adapter utilities for bridging Atlas' ZImg (modern I/O) with legacy ZStack-based algorithms.
//
// Important: these functions must never silently truncate, resample, or resize image data. If a source image is too
// large to be represented by legacy ZStack/Mc_Stack (which uses 32-bit dimensions), the functions fail with a clear
// error so callers can migrate to true large-image pipelines.

// Convert ZImg to ZStack by transferring ownership of the underlying voxel buffer.
// The input img will release its voxel data and be cleared.
ZStack* imgToZStack(ZImg& img, ZStack* data = nullptr);

// Wrap ZStack voxel data as a non-owning ZImg view.
ZImg wrapZStackAsZImg(const ZStack& stack);

ZStack* readZStack(const std::string& filename, ZStack* data = nullptr, QString* error = nullptr);
ZStack* readZStack(const QStringList& fileList,
                   Dimension catDim = Dimension::Z,
                   ZStack* data = nullptr,
                   QString* error = nullptr);

bool writeZStack(const std::string& filename, const ZStack& stack, QString* error = nullptr);

} // namespace nim
