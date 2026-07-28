// OpenBrowser — platform-specific default-browser launch (see OpenBrowser.h).

#include "common/OpenBrowser.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

namespace slopsim {
void openBrowser(const std::string& url) {
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
}  // namespace slopsim

#else
#include <cstdlib>

namespace slopsim {
void openBrowser(const std::string& url) {
    std::system(("xdg-open \"" + url + "\" >/dev/null 2>&1 &").c_str());
}
}  // namespace slopsim
#endif
