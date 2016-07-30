#ifndef ZSTRINGUTILS_H
#define ZSTRINGUTILS_H

#include <QString>

namespace nim {

bool naturalSortLessThan(const QString& s1, const QString& s2);

bool lastIntegerLessThan(const QString& s1, const QString& s2);

QString replaceLastInteger(const QString& str, const QString& replacement = "");

// checkSpecialNumber takes care of 1.#qnan, 1.#ind ... when "#" is start of comment
void removeComment(std::string& line, const std::string& commentStart = "#", bool checkSpecialNumber = true);

void removeComment(QString& line, const QString& commentStart = "#", bool checkSpecialNumber = true);

} // namespace nim

#endif // ZSTRINGUTILS_H
