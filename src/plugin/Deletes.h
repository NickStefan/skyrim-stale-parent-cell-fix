#pragma once

// History of references being marked deleted (TESForm::SetDelete, vfunc 0x23, as overridden by
// TESObjectREFR and Actor), with call stacks, so a deleted leftover found in a worldspace's
// persistent lists can be traced back to whoever deleted it.
namespace StaleParentCellFix::Deletes
{
    void Install();

    // "deleted 5321 ms ago on main thread (formID FF002527, base FE64B800, flags 00010408, cell
    // 00008ED7); stack then: ..." for the newest deletion of this reference, or a note that none
    // was recorded.
    std::string Describe(std::uintptr_t reference);
}
