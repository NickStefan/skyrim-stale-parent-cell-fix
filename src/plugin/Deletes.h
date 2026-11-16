#pragma once

// History of references being marked deleted (TESForm::SetDelete, vfunc 0x23, as overridden by
// TESObjectREFR and Actor) and of references having their formID set to 0 (TESForm::SetFormID,
// ID 14666), with call stacks, so a leftover found in a worldspace's persistent lists can be
// traced back to whoever deleted it and released its formID.
namespace StaleParentCellFix::Deletes
{
    void Install();

    // "deleted 5321 ms ago on main thread (formID FF002527, base FE64B800, flags 00010408, cell
    // 00008ED7); stack then: ..." for the newest deletion of this reference, or a note that none
    // was recorded; then the same for its formID being set to 0, if it was or is 0.
    std::string Describe(std::uintptr_t reference);
}
