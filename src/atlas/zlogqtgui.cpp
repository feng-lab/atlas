#include "zlogqtgui.h"

namespace nim {
namespace {

static_assert(fmt::is_formattable<QKeySequence>(), "QKeySequence should be formattable");
static_assert(absl::HasAbslStringify<QKeySequence>::value, "QKeySequence should support AbslStringify");
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
static_assert(fmt::is_formattable<QKeyCombination>(), "QKeyCombination should be formattable");
static_assert(absl::HasAbslStringify<QKeyCombination>::value, "QKeyCombination should support AbslStringify");
#endif

} // namespace
} // namespace nim
