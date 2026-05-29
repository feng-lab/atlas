#pragma once

#include "zlog.h"

#include <QKeySequence>
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QKeyCombination>
#endif

namespace fmt {

template<>
struct formatter<QKeySequence, char> : formatter<fmt::string_view>
{
  auto format(const QKeySequence& v, format_context& ctx) const
  {
    const auto u8 = v.toString(QKeySequence::NativeText).toUtf8();
    return formatter<fmt::string_view>::format(fmt::string_view(u8.data(), u8.size()), ctx);
  }
};

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
template<>
struct formatter<QKeyCombination, char> : formatter<fmt::string_view>
{
  auto format(const QKeyCombination& v, format_context& ctx) const
  {
    const QKeySequence seq(v);
    const auto u8 = seq.toString(QKeySequence::NativeText).toUtf8();
    return formatter<fmt::string_view>::format(fmt::string_view(u8.data(), u8.size()), ctx);
  }
};
#endif

} // namespace fmt

template<typename Sink>
void AbslStringify(Sink& sink, const QKeySequence& v)
{
  nim::appendFmtToAbslSink(sink, v);
}

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
template<typename Sink>
void AbslStringify(Sink& sink, const QKeyCombination& v)
{
  nim::appendFmtToAbslSink(sink, v);
}
#endif
