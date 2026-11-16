#include "core/Guarded.h"

#include <cstdio>
#include <cstring>

#include <Windows.h>

struct Base
{
    virtual ~Base() = default;
};

namespace RE
{
    struct TESObjectREFR : Base
    {
        int value = 7;
    };
}

namespace
{
    int failures = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        failures += ok ? 0 : 1;
    }

    template <class T>
    std::uintptr_t Vtable(const T& object)
    {
        return *reinterpret_cast<const std::uintptr_t*>(&object);
    }
}

int main()
{
    using namespace StaleParentCellFix::Core;

    std::uint64_t value = 0x1122334455667788, out = 0;
    Check(GuardedCopy(&value, &out, sizeof(out)) && out == value, "reads mapped memory");

    Check(!GuardedCopy(reinterpret_cast<const void*>(0x10), &out, sizeof(out)), "null page faults are caught");

    auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    std::memset(page, 0xAB, 4096);
    VirtualFree(page, 0, MEM_RELEASE);
    Check(!GuardedCopy(page, &out, sizeof(out)), "freed pages are caught");

    auto* guarded = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD old = 0;
    VirtualProtect(guarded + 4096, 4096, PAGE_NOACCESS, &old);
    Check(!GuardedCopy(guarded + 4092, &out, sizeof(out)), "reads straddling a no-access page are caught");
    Check(GuardedCopy(guarded + 4088, &out, sizeof(out)), "reads up to the page end succeed");

    const RE::TESObjectREFR reference;
    const auto name = RttiClassName(Vtable(reference));
    std::printf("     class name: '%s'\n", name.c_str());
    Check(name == "RE::TESObjectREFR", "RTTI class name of a namespaced class");
    Check(RttiClassName(reinterpret_cast<std::uintptr_t>(&value) + 8).empty(), "no RTTI behind a non-vtable");
    Check(RttiClassName(0x18).empty(), "unreadable vtable gives an empty name");

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
