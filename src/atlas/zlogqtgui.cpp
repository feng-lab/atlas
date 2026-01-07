#include "zlogqtgui.h"

#include <fmt/core.h>

namespace nim {
namespace {

static_assert(fmt::is_formattable<QKeySequence>(), "QKeySequence should be formattable");
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
static_assert(fmt::is_formattable<QKeyCombination>(), "QKeyCombination should be formattable");
#endif

} // namespace
} // namespace nim

