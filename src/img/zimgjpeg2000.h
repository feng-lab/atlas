#pragma once

#include "zimgformat.h"

namespace nim {

class ZImgJpeg2000 : public ZImgFormat
{
public:
  ZImgJpeg2000();
  ~ZImgJpeg2000();

  // ZImgFormat interface
public:
  virtual QString shortName() const;
  virtual QString fullName() const;
  virtual QStringList extensions() const;
  virtual void readInfo(const QString &filename, ZImgInfo &info);
  virtual void readMetadata(const QString &filename, ZImgMetadata &meta);
  virtual void readThumbnail(const QString &filename, ZImgThumbernail &thumbnail, const ZImgRegion &region);
  virtual void readImg(const QString &filename, ZImg &img, const ZImgRegion &region);
  virtual void writeImg(const QString &filename, const ZImg &img, Compression comp);
  virtual bool supportRead() const;
  virtual bool supportWrite() const;

protected:
};

} // namespace

