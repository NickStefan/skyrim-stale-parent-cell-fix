#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// CommonLib-free primitives behind Memory, so they can be tested outside the game.
namespace StaleParentCellFix::Core
{
    // memcpy that returns false instead of faulting on unmapped or protected memory.
    bool GuardedCopy(const void* source, void* out, std::size_t size) noexcept;

    // MSVC RTTI class name for a vtable ("TESObjectREFR", "BSScript::Internal::VirtualMachine"),
    // read through GuardedCopy; empty if the vtable has no RTTI.
    std::string RttiClassName(std::uintptr_t vtable);
}
