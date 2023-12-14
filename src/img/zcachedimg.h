#pragma once

#include "zimgsliceprovider.h"

namespace nim {

class ZImageCompositeTransform;

class ZCachedImg : public ZImgSliceProvider
{
public:
  explicit ZCachedImg(ZImgSource imgSource);

  ~ZCachedImg() override = default;

  [[nodiscard]] inline const ZImgInfo& info() const
  {
    return m_imgInfo;
  }

  [[nodiscard]] inline size_t width() const
  {
    return m_imgInfo.width;
  }

  [[nodiscard]] inline size_t height() const
  {
    return m_imgInfo.height;
  }

  [[nodiscard]] inline size_t depth() const
  {
    return m_imgInfo.depth;
  }

  [[nodiscard]] inline size_t numChannels() const
  {
    return m_imgInfo.numChannels;
  }

  [[nodiscard]] inline size_t numTimes() const
  {
    return m_imgInfo.numTimes;
  }

  void save(const QString& filename,
            FileFormat format = FileFormat::Unknown,
            const ZImgWriteParameters& paras = ZImgWriteParameters()) const;

  [[nodiscard]] ZImg slice(size_t z, size_t c, size_t t) const;

  void setSliceTransform(const std::map<size_t, std::unique_ptr<ZImageCompositeTransform>>* sliceTransform)
  {
    m_sliceTransform = sliceTransform;
  }

  // ZImgSliceProvider interface

public:
  [[nodiscard]] ZImgInfo imgInfo() const override
  {
    return m_imgInfo;
  }

  [[nodiscard]] ZImg slice(size_t z, size_t t) const override;

private:
  ZImgInfo m_imgInfo;
  ZImgSource m_imgSource;
  const std::map<size_t, std::unique_ptr<ZImageCompositeTransform>>* m_sliceTransform = nullptr;
};

} // namespace nim
