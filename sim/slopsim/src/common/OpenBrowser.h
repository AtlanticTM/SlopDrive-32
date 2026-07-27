#pragma once

#include <string>

namespace slopsim {

// Opens `url` in the user's default browser (fire-and-forget). Isolated in its
// own TU because the Windows path drags in <windows.h>.
void openBrowser(const std::string& url);

}  // namespace slopsim
