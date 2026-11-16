#pragma once

// Reads of pointers that may be dangling: non-canonical addresses are refused outright, the rest
// go through an SEH guard, so a freed or garbage object never faults here.
namespace StaleParentCellFix::Memory
{
    bool Plausible(std::uintptr_t address) noexcept;
    bool Read(std::uintptr_t address, void* out, std::size_t size) noexcept;

    template <class T>
    std::optional<T> Read(std::uintptr_t address) noexcept
    {
        T value{};
        if (!Read(address, &value, sizeof(T))) {
            return std::nullopt;
        }
        return value;
    }

    // True while this thread is inside Read, so the exception probe ignores our own guarded faults.
    bool Probing() noexcept;

    // MSVC RTTI class name of a vtable, e.g. "TESObjectREFR"; empty if it has none.
    std::string ClassName(std::uintptr_t vtable);

    // "SkyrimSE.exe+2CADA3 (ID 20539+0x73)" or "Some.dll+1234".
    std::string Symbol(std::uintptr_t address);

    std::string HexDump(std::uintptr_t address, std::size_t size, std::size_t indent);

    bool InGameImage(std::uintptr_t address) noexcept;
}
