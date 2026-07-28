#pragma once

// OpenBrowser — opens a URL in the user's default browser.
// Constraints:
//   Fire-and-forget, never blocks. Isolated in its own TU because the
//   Windows implementation drags in <windows.h>.

#include <string>

namespace slopsim {

void openBrowser(const std::string& url);

}  // namespace slopsim
