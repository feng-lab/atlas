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
void readStream(std::istream &fs, char *buf, size_t count);
// write seems fine
void writeStream(std::ostream &fs, const char *buf, size_t count);

QList<QStringList> readCSV(const QString filename, QChar separator = ',', QTextCodec* codec = 0);

}

#endif // ZIOUTILS_H
