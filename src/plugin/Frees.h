#pragma once

// Lifetime history of cells and references: recent cell destructions (with call stacks) and
// reference destructions, so a dangling pointer can be traced back to what used to live there,
// who destroyed it and when.
namespace StaleParentCellFix::Frees
{
    void Install();

    // "FREED: +0x128 into TESObjectCELL 0x... (formID ...) destroyed 532 ms ago on main thread"
    // with the call stack; empty if the address is in no recently destroyed cell or reference.
    std::string Describe(std::uintptr_t address);

    // One line without call stacks, for scanning many values (the exception probe).
    std::string Brief(std::uintptr_t address);
}
