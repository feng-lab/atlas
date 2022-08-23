#pragma once

#include <QIODevice>
#include <fstream>
#include <iostream>
#include <memory>

namespace nim {

// safe io functions, throw exceptions if error
void openFileStream(std::ifstream& fs,
                    const QString& filename,
                    std::ios_base::openmode mode = std::ios_base::in | std::ios_base::binary);

void openFileStream(std::ofstream& fs,
                    const QString& filename,
                    std::ios_base::openmode mode = std::ios_base::out | std::ios_base::binary);

// mac: if count >= 2G, fs.read() will fail, this is a workaround
void readStream_impl(std::istream& fs, char* buf, size_t count);

template<typename T>
inline void readStream(std::istream& fs, T* buf, size_t count)
{
  // reinterpret_cast allowed (AliasedType is char or unsigned char: this permits
  // examination of the object representation of any object as an array of unsigned char.)
  readStream_impl(fs, reinterpret_cast<char*>(buf), count);
}

// write seems fine
void writeStream_impl(std::ostream& fs, const char* buf, size_t count);

template<typename T>
inline void writeStream(std::ostream& fs, const T* buf, size_t count)
{
  // reinterpret_cast allowed (AliasedType is char or unsigned char: this permits
  // examination of the object representation of any object as an array of unsigned char.)
  writeStream_impl(fs, reinterpret_cast<const char*>(buf), count);
}

#ifdef _MSC_VER

std::unique_ptr<std::FILE, decltype(&std::fclose)> openFile(const QString& filename, const QString& mode);

#else

std::unique_ptr<std::FILE, decltype(&std::fclose)> openFile(const QString& filename, const char* mode);

#endif

QString getTemporaryFilename(const QString& filename);

void renameFile(const QString& oldName, const QString& newName);

std::string readFileIntoString(const QString& filename,
                               std::ios_base::openmode mode = std::ios_base::in | std::ios_base::binary);

QByteArray readFileIntoByteArray(const QString& filename, QIODevice::OpenMode openMode = QIODevice::ReadOnly);

} // namespace nim
