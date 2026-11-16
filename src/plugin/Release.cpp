#include "plugin/Release.h"

#include "plugin/Detour.h"
#include "plugin/Forms.h"
#include "plugin/Memory.h"
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
            if (reference && reference->formID == 0) {
                const auto address = reinterpret_cast<std::uintptr_t>(reference);
                auto* cell = reinterpret_cast<RE::TESObjectCELL*>(
                    Memory::Read<std::uintptr_t>(address + Forms::Offset::RefParentCell).value_or(0));
                if (cell) {
                    const auto cellFormID = cell->formID;
                    if (removeFromCell) {
                        static REL::Relocation<RemoveReferenceFn> removeReference{ REL::ID(19032) };
                        removeReference(cell, reference);
                    }
                    // vfunc 0x98, as 36459 itself calls it.
                    const auto vtable = *reinterpret_cast<std::uintptr_t* const*>(reference);
                    reinterpret_cast<SetParentCellFn>(vtable[0x98])(reference, nullptr);

                    const auto count = ++completed;
                    if (count <= DetailedLines) {
                        const auto base = reference->data.objectReference;
                        spdlog::warn("release of an already released reference 0x{:X} (formID 0, base {:08X}, flags "
                                     "{:08X}) still pointing at cell {:08X} (0x{:X}): {}cleared its parentCell, which "
                                     "the game skips (#{})",
                            address, base ? base->formID : 0, reference->formFlags, cellFormID,
                            reinterpret_cast<std::uintptr_t>(cell), removeFromCell ? "removed it from the cell and " : "",
                            count);
                    } else if (count == DetailedLines + 1) {
                        spdlog::info("further releases completed this way are counted, not logged");
                    }
                    Report::Notice("completed", "StaleParentCellFix: fixed a reference at the source of the crash. "
                                                "Please send StaleParentCellFix.log.");
                }
            }
            release(manager, reference, removeFromCell);
        }
    }

    void Install()
    {
        release = Detour::Entry("ReleaseReference", REL::ID(36459),
            std::array<std::uint8_t, 8>{ 0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x57, 0x41, 0x54 }, &Release);
        spdlog::info(release ? "completing the release of already released references that still point at a cell"
                             : "release not hooked; relying on the destroy-time fix alone");
    }
}
