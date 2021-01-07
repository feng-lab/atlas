#pragma once

#include "zexception.h"

namespace nim {

QString randomString(int minLength, int maxLength);

inline QString randomString(int size)
{ return randomString(size, size); }

bool naturalSortLessThan(const QString& s1, const QString& s2);

bool lastIntegerLessThan(const QString& s1, const QString& s2);

QString replaceLastInteger(const QString& str, const QString& replacement = "");

// checkSpecialNumber takes care of 1.#qnan, 1.#ind ... when "#" is start of comment
void removeComment(std::string& line, const std::string& commentStart = "#", bool checkSpecialNumber = true);

void removeComment(QString& line, const QString& commentStart = "#", bool checkSpecialNumber = true);

class QStringNaturalCompare
{
public:
  inline bool operator()(const QString& s1, const QString& s2) const
  {
    return naturalSortLessThan(s1, s2);
  }
};

} // namespace nim

