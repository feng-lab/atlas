#include "zimgstackinterface.h"

#include "zexception.h"
#include "zimg.h"
#include "zlog.h"

#include <boost/align/aligned_alloc.hpp>

#include <cstdlib>
#include <limits>

namespace nim {

namespace {

void alignedKill(Mc_Stack* stack)
{
  if (stack == nullptr) {
    return;
  }
  boost::alignment::aligned_free(stack->array);
  std::free(stack);
}

[[nodiscard]] int checkedIntFromSize(size_t value, const char* label)
{
  if (value > static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw ZException(fmt::format("Image dimension '{}'={} exceeds legacy ZStack limit (int32).", label, value));
  }
  return static_cast<int>(value);
}

[[nodiscard]] int zstackKindFromImgOrThrow(const ZImg& img)
{
  if (img.voxelFormat() == VoxelFormat::Signed) {
    throw ZException("Signed integer voxel format is not supported by the legacy ZStack adapter.");
  }

  const size_t bytes = img.bytesPerVoxel();
  if (bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8) {
    throw ZException(fmt::format("Unsupported bytes per voxel for ZStack adapter: {}", bytes));
  }

  // Legacy stack kinds are effectively "bytes per voxel" for GREY/GREY16/FLOAT32/FLOAT64 in this codebase.
  // (Color kinds are not supported by neuTube tracing anyway.)
  return static_cast<int>(bytes);
}

[[nodiscard]] size_t checkedMulSize(size_t a, size_t b, const char* label)
{
  if (a != 0 && b > std::numeric_limits<size_t>::max() / a) {
    throw ZException(fmt::format("Image size overflow while computing {} ({} * {}).", label, a, b));
  }
  return a * b;
}

struct McStackHolder
{
  Mc_Stack* stack = nullptr;

  McStackHolder() = default;

  McStackHolder(const McStackHolder&) = delete;
  McStackHolder& operator=(const McStackHolder&) = delete;

  ~McStackHolder()
  {
    if (stack != nullptr) {
      alignedKill(stack);
    }
  }

  void release()
  {
    stack = nullptr;
  }
};

} // namespace

ZStack* imgToZStack(ZImg& img, ZStack* data)
{
  if (img.isEmpty()) {
    throw ZException("Cannot convert empty ZImg to ZStack.");
  }

  if (img.numTimes() != 1) {
    throw ZException(fmt::format("ZStack adapter does not support numTimes={}, expected 1.", img.numTimes()));
  }

  if (img.isImgView()) {
    throw ZException("ZStack adapter cannot take ownership from a ZImg view; provide an owning ZImg instead.");
  }

  const size_t numChannels = img.numChannels();

  const int width = checkedIntFromSize(img.width(), "width");
  const int height = checkedIntFromSize(img.height(), "height");
  const int depth = checkedIntFromSize(img.depth(), "depth");
  const int nchannel = checkedIntFromSize(numChannels, "numChannels");
  const int kind = zstackKindFromImgOrThrow(img);

  const size_t expectedBytes = img.timeByteNumber();
  const size_t computedBytes = checkedMulSize(
    checkedMulSize(checkedMulSize(checkedMulSize(static_cast<size_t>(kind), static_cast<size_t>(width), "kind*width"),
                                  static_cast<size_t>(height),
                                  "kind*width*height"),
                   static_cast<size_t>(depth),
                   "kind*width*height*depth"),
    static_cast<size_t>(nchannel),
    "kind*width*height*depth*nchannel");
  if (computedBytes != expectedBytes) {
    throw ZException(fmt::format("Image byte size mismatch: computed={} expected={}.", computedBytes, expectedBytes));
  }

  McStackHolder holder;
  holder.stack = static_cast<Mc_Stack*>(std::malloc(sizeof(Mc_Stack)));
  if (holder.stack == nullptr) {
    throw ZException("Out of memory allocating Mc_Stack header.");
  }

  holder.stack->kind = kind;
  holder.stack->width = width;
  holder.stack->height = height;
  holder.stack->depth = depth;
  holder.stack->nchannel = nchannel;
  holder.stack->array = img.timeData<uint8_t>(0);

  img.releaseTimeData(0);

  if (data == nullptr) {
    data = new ZStack();
  }

  data->setData(holder.stack, alignedKill);
  holder.release();

  img.clear();

  return data;
}

ZImg wrapZStackAsZImg(const ZStack& stack)
{
  ZImg img;

  switch (stack.kind()) {
    case 1:
      img.wrapData(reinterpret_cast<uint8_t*>(stack.mc_stack()->array),
                   static_cast<size_t>(stack.width()),
                   static_cast<size_t>(stack.height()),
                   static_cast<size_t>(stack.depth()),
                   static_cast<size_t>(stack.channelNumber()));
      break;
    case 2:
      img.wrapData(reinterpret_cast<uint16_t*>(stack.mc_stack()->array),
                   static_cast<size_t>(stack.width()),
                   static_cast<size_t>(stack.height()),
                   static_cast<size_t>(stack.depth()),
                   static_cast<size_t>(stack.channelNumber()));
      break;
    case 4:
      img.wrapData(reinterpret_cast<float*>(stack.mc_stack()->array),
                   static_cast<size_t>(stack.width()),
                   static_cast<size_t>(stack.height()),
                   static_cast<size_t>(stack.depth()),
                   static_cast<size_t>(stack.channelNumber()));
      break;
    case 8:
      img.wrapData(reinterpret_cast<double*>(stack.mc_stack()->array),
                   static_cast<size_t>(stack.width()),
                   static_cast<size_t>(stack.height()),
                   static_cast<size_t>(stack.depth()),
                   static_cast<size_t>(stack.channelNumber()));
      break;
    default:
      throw ZException(fmt::format("Unsupported ZStack kind {} for wrapZStackAsZImg.", stack.kind()));
  }

  return img;
}

ZStack* readZStack(const std::string& filename, ZStack* data, QString* error)
{
  try {
    ZImg img(QString::fromStdString(filename));
    return imgToZStack(img, data);
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Failed to load image '" << filename << "': " << e.what();
    if (error != nullptr) {
      *error = e.what();
    }
  }

  return nullptr;
}

ZStack* readZStack(const QStringList& fileList, Dimension catDim, ZStack* data, QString* error)
{
  try {
    ZImg img(fileList, catDim, false);
    return imgToZStack(img, data);
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Failed to load image sequence: " << e.what();
    if (error != nullptr) {
      *error = e.what();
    }
  }
  return nullptr;
}

bool writeZStack(const std::string& filename, const ZStack& stack, QString* error)
{
  try {
    ZImg img = wrapZStackAsZImg(stack);
    img.save(QString::fromStdString(filename));
    LOG(INFO) << "Wrote image: " << filename;
    return true;
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Failed to write image '" << filename << "': " << e.what();
    if (error != nullptr) {
      *error = e.what();
    }
  }
  return false;
}

} // namespace nim
