#include "plugin/Release.h"

#include "plugin/Detour.h"
#include "plugin/Report.h"

namespace StaleParentCellFix::Release
{
    namespace
    {
        // ID 36459: (manager, reference, removeFromCell).
        using ReleaseFn = void (*)(void*, RE::TESObjectREFR*, bool);
        ReleaseFn release = nullptr;

        using RemoveReferenceFn = void (*)(RE::TESObjectCELL*, RE::TESObjectREFR*);
        using SetParentCellFn = void (*)(RE::TESObjectREFR*, RE::TESObjectCELL*);

        std::atomic<std::uint64_t> completed = 0;
        constexpr std::uint64_t DetailedLines = 50;

        void Release(void* manager, RE::TESObjectREFR* reference, bool removeFromCell)
        {
            if (reference && reference->formID == 0 && reference->parentCell) {
                auto* cell = reference->parentCell;
                if (removeFromCell) {
                    static REL::Relocation<RemoveReferenceFn> removeReference{ REL::ID(19032) };
                    removeReference(cell, reference);
                }
                // vfunc 0x98, as 36459 itself calls it.
                const auto vtable = *reinterpret_cast<std::uintptr_t* const*>(reference);
                reinterpret_cast<SetParentCellFn>(vtable[0x98])(reference, nullptr);

                const auto count = ++completed;
                if (count <= DetailedLines) {
                    const auto* base = reference->data.objectReference;
                    const auto& position = reference->data.location;
                    spdlog::info("fixed released reference 0x{:X} (base {:08X}, flags {:08X}, position ({:.0f}, "
                                 "{:.0f}, {:.0f})) still pointing at cell {:08X}: {}cleared its parentCell (#{})",
                        reinterpret_cast<std::uintptr_t>(reference), base ? base->formID : 0, reference->formFlags,
                        position.x, position.y, position.z, cell->formID,
                        removeFromCell ? "removed it from the cell and " : "", count);
                } else if (count == DetailedLines + 1) {
                    spdlog::info("further fixes are counted, not logged");
                }
                Report::Notice("fixed", "StaleParentCellFix: fixed a stale cell reference (see StaleParentCellFix.log).");
            }
            release(manager, reference, removeFromCell);
        }
    }

    void Install()
    {
        release = Detour::Entry("ReleaseReference", REL::ID(36459),
            std::array<std::uint8_t, 8>{ 0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x57, 0x41, 0x54 }, &Release);
        spdlog::info(release ? "completing the release of already released references that still point at a cell"
                             : "release not hooked; the fix is inactive");
    }
}
