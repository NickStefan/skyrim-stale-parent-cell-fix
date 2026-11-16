#include "plugin/Detour.h"

namespace StaleParentCellFix::Detour
{
    namespace
    {
        std::string Bytes(std::uintptr_t address, std::size_t size)
        {
            std::string out;
            for (std::size_t i = 0; i < size; ++i) {
                out += std::format("{}{:02X}", i ? " " : "", *reinterpret_cast<const std::uint8_t*>(address + i));
            }
            return out;
        }
    }

    std::uintptr_t Entry(std::string_view name, REL::ID id, std::span<const std::uint8_t> expected, std::uintptr_t hook)
    {
        const auto target = id.address();
        if (expected.size() < 5 || std::memcmp(reinterpret_cast<const void*>(target), expected.data(), expected.size())) {
            spdlog::error("{} (ID {}): unexpected bytes {} at entry; not hooked (another mod may patch it)", name,
                id.id(), Bytes(target, expected.size()));
            return 0;
        }
        auto& trampoline = SKSE::GetTrampoline();
        auto* gate = static_cast<std::uint8_t*>(trampoline.allocate(expected.size() + 14));
        std::memcpy(gate, expected.data(), expected.size());
        // jmp [rip+0]; dq target+prologue
        const std::uint8_t jump[6]{ 0xFF, 0x25, 0, 0, 0, 0 };
        std::memcpy(gate + expected.size(), jump, sizeof(jump));
        const auto back = target + expected.size();
        std::memcpy(gate + expected.size() + sizeof(jump), &back, sizeof(back));

        trampoline.write_branch<5>(target, hook);
        for (std::size_t i = 5; i < expected.size(); ++i) {
            REL::safe_write(target + i, std::uint8_t{ 0xCC });
        }
        spdlog::info("{} (ID {}, SkyrimSE.exe+{:X}) hooked", name, id.id(), id.offset());
        return reinterpret_cast<std::uintptr_t>(gate);
    }
}
