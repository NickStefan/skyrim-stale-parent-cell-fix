#pragma once

// The fix. A worldspace keeps counted pointers to its persistent references (fixed ones per cell,
// mobile ones in one list), but destroying an exterior cell never clears those references'
// parentCell. The next time the cell loads, the loader (ID 20532) re-adds them and removes them
// from the freed cell first; quests checking their location read the freed cell too. Before a
// cell is destroyed this clears parentCell on the formID 0 or deleted persistent references of its
// worldspace that still point at it. A null parentCell is a state the loader already handles.
//
// The references it leaves pointing at the cell (all of them with the fix off) are remembered, so
// the guards can tell when the game later reads one of those stale pointers.
namespace StaleParentCellFix::Unlink
{
    struct Stale
    {
        std::uintptr_t cell;
        std::uint32_t cellFormID;
        std::uint64_t time;  // GetTickCount64 when the cell was destroyed
        std::uint32_t formID;
        std::uint32_t baseFormID;
        std::uint32_t flags;
    };

    // With `clear` false nothing is changed; references are only remembered.
    void Install(bool clear);

    // The record for `reference` if it still holds the pointer it was left with when that cell was
    // destroyed; `forget` drops the record (the pointer is about to be replaced).
    std::optional<Stale> FindStale(std::uintptr_t reference, std::uintptr_t parentCell, bool forget);
}
