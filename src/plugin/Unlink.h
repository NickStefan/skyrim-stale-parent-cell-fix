#pragma once

// The fix. A worldspace keeps counted pointers to its persistent references (fixed ones per cell,
// mobile ones in one list). Destroying an exterior cell nulls parentCell only on the references in
// the cell's own reference set (19410 -> 19458 -> 19468), so one that points at the cell from the
// persistent lists but has already left that set keeps pointing at freed memory; the next time
// the cell loads, the loader (ID 20532) re-adds it and removes it from the freed cell first.
// Before a cell is destroyed this clears parentCell on exactly those references. A null
// parentCell is a state the loader already handles.
//
// After the destructor it checks that every reference it left alone was nulled, and logs any that
// were not. It also logs the first sighting of each formID 0 persistent reference.
namespace StaleParentCellFix::Unlink
{
    // With `clear` false nothing is changed; references are only checked and logged.
    void Install(bool clear);

    // Whether `reference` is in the cell's own reference set (read without the cell's lock).
    bool InCellSet(std::uintptr_t cell, std::uintptr_t reference);

    struct CellSetState
    {
        std::uintptr_t entries;  // the table's slot array; a new one means it was rebuilt
        std::uint32_t capacity;
        std::uint32_t free;
        bool contains;
    };
    CellSetState ReadCellSet(std::uintptr_t cell, std::uintptr_t reference);
}
