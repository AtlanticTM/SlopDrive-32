#pragma once

// MachineScreen — CLI-flavored FTXUI panel for the virtual SlopDrive.
// Constraints:
//   Runs the sim and the FTXUI loop on ONE thread (ftxui::Loop::RunOnce
//   inside the sim tick loop), preserving the one-task invariant.

#include "common/SessionLog.h"
#include "machine/MachineSim.h"

namespace slopsim {

// Blocks until the user quits (q / Ctrl-C). Returns process exit code.
// httpPort feeds the /graph analyzer URL the `analyze` command opens.
int runMachineScreen(MachineSim& sim, SessionLog& log, uint16_t wsPort, uint16_t httpPort);

}  // namespace slopsim
