#pragma once

namespace StaleParentCellFix::Detour
{
    // Redirects a function's entry to `hook` and returns a gate that runs the original (its
    // relocated prologue, then a jump back). The prologue must hold no relative addressing and
    // match `expected` exactly; otherwise (another mod patched it, another runtime) nothing is
    // written and 0 is returned.
    std::uintptr_t Entry(std::string_view name, REL::ID id, std::span<const std::uint8_t> expected, std::uintptr_t hook);

    // Redirects a 5-byte E8 call; returns the original callee, or 0 if the site is not that call.
    std::uintptr_t Call(std::string_view name, REL::ID id, std::ptrdiff_t offset, std::uintptr_t expectedCallee,
        std::uintptr_t hook);

    template <class F>
    F Entry(std::string_view name, REL::ID id, std::span<const std::uint8_t> expected, F hook)
    {
        return reinterpret_cast<F>(Entry(name, id, expected, reinterpret_cast<std::uintptr_t>(hook)));
    }
}
