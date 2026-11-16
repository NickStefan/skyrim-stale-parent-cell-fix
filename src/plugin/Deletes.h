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
    // Then each time it was given a parentCell while its formID was 0 (TESObjectREFR::SetParentCell,
    // ID 19818), and whether it was in that cell's reference list right after.
    std::string Describe(std::uintptr_t reference);

    // Checks now whether a formID 0 reference put into a cell's set has dropped out of it without
    // RemoveReference (also checked every 20 ms or so from the hooks), logging the first time.
    void Check();

    // Keeps this reference's current deletion and formID records past the rings' turnover.
    void Pin(std::uintptr_t reference);
}
