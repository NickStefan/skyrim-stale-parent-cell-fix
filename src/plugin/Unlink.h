#pragma once

// The fix. A worldspace keeps counted pointers to its persistent references (fixed ones per cell,
// mobile ones in one list), but destroying an exterior cell never clears those references'
// parentCell. The next time the cell loads, the loader (ID 20532) re-adds them and removes them
// from the freed cell first; quests checking their location read the freed cell too. Before a
// cell is destroyed this clears parentCell on the formID 0 or deleted persistent references of its
// worldspace that still point at it. A null parentCell is a state the loader already handles.
namespace StaleParentCellFix::Unlink
{
    void Install();
}
