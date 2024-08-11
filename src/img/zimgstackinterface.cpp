#ifdef _NEUTUBE_

#include "zimgstackinterface.h"

#include "zimg.h"
#include "zlog.h"
#include <QDir>

#include <boost/align/aligned_alloc.hpp>

namespace nim {

void alignedKill(Mc_Stack* stack)
{
  if (stack) {
    boost::alignment::aligned_free(stack->array);
    free(stack);
  }
}

ZStack* imgToZStack(ZImg& img, ZStack* data)
{
  Mc_Stack* stack = (Mc_Stack*)malloc(sizeof(Mc_Stack));
  stack->array = img.timeData<uint8_t>(0);
  stack->width = img.width();
  stack->height = img.height();
  stack->kind = img.voxelByteNumber();
  stack->nchannel = img.numChannels();
  stack->depth = img.depth();
  img.releaseTimeData(0);
  if (!data) {
    data = new ZStack();
  }
  data->setData(stack, alignedKill);
  data->initChannelColors();
  for (size_t i = 0; i < img.numChannels(); ++i) {
    data->setChannelColor(i, img.channelColor(i).r, img.channelColor(i).g, img.channelColor(i).b);
  }
  if (img.voxelSizeUnit() == VoxelSizeUnit::Voxel) {
    data->setResolution(img.voxelSizeX(), img.voxelSizeY(), img.voxelSizeZ(), 'p');
  } else {
    data->setResolution(img.voxelSizeXInUm(), img.voxelSizeYInUm(), img.voxelSizeZInUm(), 'u');
  }
  img.clear();
  return data;
}

ZImg wrapZStackAsZImg(const ZStack& stack)
{
  ZImg res;
  switch (stack.kind()) {
    case 1:
      res.wrapData((uint8_t*)(stack.mc_stack()->array),
                   stack.width(),
                   stack.height(),
                   stack.depth(),
                   stack.channelNumber());
      break;
    case 2:
      res.wrapData((uint16_t*)(stack.mc_stack()->array),
                   stack.width(),
                   stack.height(),
                   stack.depth(),
                   stack.channelNumber());
      break;
    case 4:
      res.wrapData((float*)(stack.mc_stack()->array),
                   stack.width(),
                   stack.height(),
                   stack.depth(),
                   stack.channelNumber());
      break;
    case 8:
      res.wrapData((double*)(stack.mc_stack()->array),
                   stack.width(),
                   stack.height(),
                   stack.depth(),
                   stack.channelNumber());
      break;
    default:
      break;
  }
  if (stack.resolution().unit() == 'u') {
    res.infoRef().voxelSizeX = stack.resolution().voxelSizeX();
    res.infoRef().voxelSizeY = stack.resolution().voxelSizeY();
    res.infoRef().voxelSizeZ = stack.resolution().voxelSizeZ();
  }
  return res;
}

ZStack* readZStack(const std::string& filename, ZStack* data, QString* error)
{
  try {
    ZImg img(QString::fromStdString(filename));

    // workaround QImage limit
    if (img.width() * img.height() * 4 >= 1024_uz * 1024 * 1024 * 2) {
      double scale = (1024.0 * 1024 * 1024 * 2) / ((double)img.width() * img.height() * 4);
      size_t newWidth = static_cast<size_t>(std::floor(img.width() * scale));
      size_t newHeight = static_cast<size_t>(std::floor(img.height() * scale));
      ZImgInfo oldInfo = img.info();
      img.resize(newWidth, newHeight, img.depth(), Interpolant::Cubic);
      LOG(WARNING) << fmt::format("Image <{}> is too big for Qt to display, resize to <{}>", oldInfo, img.info());
    }

    return imgToZStack(img, data);
  }
  catch (const ZException& e) {
    LOG(ERROR) << e.what();
    if (error) {
      *error = e.what();
    }
  }
  return nullptr;
}

bool writeZStack(const std::string& filename, const ZStack& stack, QString* error)
{
  ZImg img = wrapZStackAsZImg(stack);

  try {
    img.save(QString::fromStdString(filename));
    LOG(INFO) << "Wrote image: " << filename;
    return true;
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to write image " << filename << ". error: " << e.what();
    if (error) {
      *error = e.what();
    }
  }
  return false;
}

ZStack* readZStack(const QStringList& fileList, Dimension catDim, QString* error)
{
  try {
    ZImg img;
    img.loadSequence(fileList, catDim);
    // workaround QImage limit
    if (img.width() * img.height() * 4 >= 1024_uz * 1024 * 1024 * 2) {
      double scale = (1024.0 * 1024 * 1024 * 2) / ((double)img.width() * img.height() * 4);
      size_t newWidth = static_cast<size_t>(std::floor(img.width() * scale));
      size_t newHeight = static_cast<size_t>(std::floor(img.height() * scale));
      ZImgInfo oldInfo = img.info();
      img.resize(newWidth, newHeight, img.depth(), Interpolant::Cubic);
      LOG(WARNING) << fmt::format("Image <{}> is too big for Qt to display, resize to <{}>", oldInfo, img.info());
    }

    return imgToZStack(img);
  }
  catch (const ZException& e) {
    LOG(ERROR) << e.what();
    if (error) {
      *error = e.what();
    }
  }
  return nullptr;
}

} // namespace nim

#endif
