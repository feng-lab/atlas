#pragma once

#include "zglobal.h"
#include "zioutils.h"
#include "zlog.h"
#include <reflect>

namespace nim {

template<class T>
  requires std::is_aggregate_v<T>
void printStruct(const T& t = {}, const std::string& memberName = "", size_t memberOffset = 0)
{
  reflect::for_each(
    [&](auto I) {
      if constexpr (std::is_aggregate_v<std::remove_cvref_t<decltype(reflect::get<I>(t))>> &&
                    !IsStdArray<std::remove_cvref_t<decltype(reflect::get<I>(t))>>::value) {
        printStruct(reflect::get<I>(t),
                    fmt::format("{}{}.{}",
                                memberName.empty() ? "" : memberName + " ",
                                reflect::type_name(t),
                                reflect::member_name<I>(t)),
                    memberOffset + reflect::offset_of<I>(t));
      } else {
        fmt::print("{}{}.{}: {} = {} ({}/{}/{}) {}\n",
                   memberName.empty() ? "" : memberName + " ",
                   reflect::type_name(t),
                   reflect::member_name<I>(t),
                   reflect::type_name(reflect::get<I>(t)),
                   reflect::get<I>(t),
                   reflect::size_of<I>(t),
                   reflect::align_of<I>(t),
                   memberOffset + reflect::offset_of<I>(t),
                   reinterpret_cast<std::uintptr_t>(&reflect::get<I>(t)));
      }
    },
    t);
}

template<class T>
  requires std::is_aggregate_v<T>
size_t compactSize(const T& t = {})
{
  size_t totalSize = 0;
  reflect::for_each(
    [&](auto I) {
      if constexpr (std::is_aggregate_v<std::remove_cvref_t<decltype(reflect::get<I>(t))>> &&
                    !IsStdArray<std::remove_cvref_t<decltype(reflect::get<I>(t))>>::value) {
        totalSize += compactSize(reflect::get<I>(t));
      } else {
        totalSize += reflect::size_of<I>(t);
      }
    },
    t);
  return totalSize;
}

// return memory size
template<class T>
  requires std::is_aggregate_v<T>
size_t compactStructToMemory(uint8_t* mem, size_t memSize, const T& t)
{
  size_t offset = 0;
  reflect::for_each(
    [&](auto I) {
      if constexpr (std::is_aggregate_v<std::remove_cvref_t<decltype(reflect::get<I>(t))>> &&
                    !IsStdArray<std::remove_cvref_t<decltype(reflect::get<I>(t))>>::value) {
        auto dataSize = compactSize(reflect::get<I>(t));
        CHECK(offset + dataSize <= memSize) << "Buffer overflow detected: " << offset + dataSize << " " << memSize;
        compactStructToMemory(mem + offset, dataSize, reflect::get<I>(t));
        offset += dataSize;
      } else {
        auto dataSize = reflect::size_of<I>(t);
        CHECK(offset + dataSize <= memSize) << "Buffer overflow detected: " << offset + dataSize << " " << memSize;
        std::memcpy(mem + offset, &reflect::get<I>(t), dataSize);
        offset += dataSize;
      }
    },
    t);
  return offset;
}

template<class T>
  requires std::is_aggregate_v<T>
void readStructFromCompactMemory(T& t, const uint8_t* mem, size_t memSize)
{
  size_t offset = 0;
  reflect::for_each(
    [&](auto I) {
      if constexpr (std::is_aggregate_v<std::remove_cvref_t<decltype(reflect::get<I>(t))>> &&
                    !IsStdArray<std::remove_cvref_t<decltype(reflect::get<I>(t))>>::value) {
        auto dataSize = compactSize(reflect::get<I>(t));
        if (offset + dataSize <= memSize) {
          readStructFromCompactMemory(reflect::get<I>(t), mem + offset, dataSize);
        } else {
          LOG(WARNING) << fmt::format("{}.{} not filled", reflect::type_name(t), reflect::member_name<I>(t));
        }
        offset += dataSize;
      } else {
        auto dataSize = reflect::size_of<I>(t);
        if (offset + dataSize <= memSize) {
          std::memcpy(&reflect::get<I>(t), mem + offset, dataSize);
        } else {
          LOG(WARNING) << fmt::format("{}.{} not filled", reflect::type_name(t), reflect::member_name<I>(t));
        }
        offset += dataSize;
      }
    },
    t);
}

template<class T>
  requires std::is_aggregate_v<T>
void readStructFromFileStream(T& t, std::istream& fs)
{
  reflect::for_each(
    [&](auto I) {
      if constexpr (std::is_aggregate_v<std::remove_cvref_t<decltype(reflect::get<I>(t))>> &&
                    !IsStdArray<std::remove_cvref_t<decltype(reflect::get<I>(t))>>::value) {
        readStructFromFileStream(reflect::get<I>(t), fs);
      } else {
        readStream(fs, &reflect::get<I>(t), reflect::size_of<I>(t));
      }
    },
    t);
}

template<class T>
  requires std::is_aggregate_v<T>
bool readStructFromFileStreamNoThrow(T& t, std::istream& fs)
{
  try {
    readStructFromFileStream(t, fs);
    return true;
  }
  catch (const ZException& e) {
    LOG(WARNING) << e.what();
  }
  return false;
}

} // namespace nim