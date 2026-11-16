#pragma once

// First-chance access violation logger for SkyrimSE.exe code: registers and stack slots that point
// into recently destroyed cells/references, plus an unwound call stack. Never handles the
// exception; CrashLogger and the game see it exactly as before.
namespace StaleParentCellFix::Probe
{
    void Install(int reports);
}
