#ifndef ZIOUTILS_H
#define ZIOUTILS_H

#include <iostream>
#include <fstream>
#include <QString>

namespace nim {

// safe io functions, throw exceptions if error
void openFileStream(std::ifstream &fs, const QString &filename, std::ios_base::openmode mode);
void openFileStream(std::ofstream &fs, const QString &filename, std::ios_base::openmode mode);

// mac: if count >= 2G, fs.read() will fail, this is a workaround
void readStream_impl(std::istream &fs, char *buf, size_t count);
template<typename T>
inline void readStream(std::istream &fs, T *buf, size_t count)
{
  readStream_impl(fs, reinterpret_cast<char*>(buf), count);
}

// write seems fine
void writeStream_impl(std::ostream &fs, const char *buf, size_t count);
template<typename T>
inline void writeStream(std::ostream &fs, const T *buf, size_t count)
{
  writeStream_impl(fs, reinterpret_cast<const char*>(buf), count);
}

}

#endif // ZIOUTILS_H
