#pragma once

// The fix. The game's reference release (ID 36459) returns at once for a reference whose formID
// is already 0, taking it for released. But a released reference left in the worldspace's mobile
// persistent list is re-added to its cell by the loader (20532), and when that cell unloads, 19113
// erases it from the cell's set (19235) and passes it to 36459 to clear its parentCell, which then
// never happens: the reference keeps pointing at the cell after the cell is destroyed. This
// finishes that part of the release for such references: it removes them from the cell if the
// caller asked for it, and nulls parentCell, before calling the original.
namespace StaleParentCellFix::Release
{
    void Install();
}
