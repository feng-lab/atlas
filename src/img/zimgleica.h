#pragma once

#include "zimgformat.h"

namespace nim {

#pragma pack(push, 1)

// Info for the next block to read out of the file
struct NextBlock
{
  int32_t test; // test == 0x70
  int32_t length; // Number of bytes to read
};

// XML Description
struct XMLContent
{
  unsigned char test; // test == 0x2A
  int32_t textLength; // Number of unicode characters
  //wchar_t text[]; // XML Object Description
};

struct TypeContent
{
  unsigned char test; // test == 0x2A
  int32_t textLength; // Number of unicode characters
  //wchar_t text[]; // Type Name: "LMS_Object_File"
};

struct Int32Block
{
  unsigned char test; // test == 0x2A
  uint32_t number; // Number of unicode characters
};

struct UInt64Block
{
  unsigned char test; // test == 0x2A
  uint64_t Number; // Number of unicode characters
};

// Memory Description for File Version 1 (32 Bit)
struct MemoryBlock32
{
  unsigned char test1; // test == 0x2A
  uint32_t memorySize; // Memory size of the object
  unsigned char test2; // test == 0x2A
  int32_t textLength; // Number of Unicode characters
  //wchar_t text[]; // Short image description (Name)
};

// Memory Description for File Version 2 (64 Bit)
struct MemoryBlock64
{
  unsigned char test1; // test == 0x2A
  uint64_t memorySize;// Memory size of the object
  unsigned char test2; // test == 0x2A
  int32_t textLength; // Number of Unicode characters
  //wchar_t text[]; // Short image description (Name)
};

#pragma pack(pop)

class ZImgLeica : public ZImgFormat
{
public:

  // ZImgFormat interface
public:
  bool supportRead() const override;

  bool supportWrite() const override;

  QString shortName() const override;

  QString fullName() const override;

  QStringList extensions() const override;

  FileFormat format() const override
  { return FileFormat::Leica; }

  void readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                std::vector<std::set<size_t>>* pyramidalRatios) override;

  void readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene) override;

  void
  readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene) override;

  void readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene, size_t ratio) override;

private:

private:

};

} // namespace



