#include "zwindowsheader.h"

#if defined(_WIN32)

#include "zlog.h"

namespace nim {

UINT getActiveCodePage()
{
  return GetACP();
}

void logActiveCodePage()
{
  auto activeCodePage = GetACP();

  CPINFOEX cpInfo;
  if (GetCPInfoEx(activeCodePage, 0, &cpInfo)) {
    LOG(INFO) << fmt::format("Activate code page: {}, code page name: {}, max char size: {} byte(s)",
                             activeCodePage,
                             toUtf8String(cpInfo.CodePageName),
                             cpInfo.MaxCharSize);
  } else {
    LOG(ERROR) << fmt::format("Activate code page: {}, failed to get code page info, error code: {}",
                              activeCodePage,
                              GetLastError());
  }
}

} // namespace nim

#endif
