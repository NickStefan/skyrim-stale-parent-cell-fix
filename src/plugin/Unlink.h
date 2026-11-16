#pragma once

// The fix. A worldspace keeps counted pointers to its persistent references (fixed ones per cell,
// mobile ones in one list). Destroying an exterior cell nulls the parentCell of those pointing at
// it, but formID 0 leftovers have been seen still pointing at it afterwards; the next time the
// cell loads, the loader (ID 20532) re-adds them and removes them from the freed cell first. Before
// a cell is destroyed this clears parentCell on the formID 0 or deleted persistent references of
// its worldspace that still point at it. A null parentCell is a state the loader already handles.
//
// After the destructor it checks that every reference it left alone was nulled, and logs any that
// were not. It also logs the first sighting of each formID 0 persistent reference.
namespace StaleParentCellFix::Unlink
{
    // With `clear` false nothing is changed; references are only checked and logged.
    void Install(bool clear);
}
