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

        // Only forms the form map still resolves to this exact object are followed, so a released
        // or freed one is never read beyond its ID.
        const RE::TESForm* Live(const RE::TESForm* form)
        {
            if (!form || form->GetFormID() == 0) {
                return nullptr;
            }
            return RE::TESForm::LookupByID(form->GetFormID()) == form ? form : nullptr;
        }

        // `NPC_ FF003D27 "Bandit" (EncBandit01Melee) created at runtime` or `... from Skyrim.esm`.
        std::string Describe(const RE::TESForm* form)
        {
            if (!form) {
                return "none";
            }
            const auto id = form->GetFormID();
            const auto* live = Live(form);
            if (!live) {
                return std::format("{:08X} (no longer in the form map)", id);
            }
            const char* name = live->GetName();
            const char* editorID = live->GetFormEditorID();
            const auto* file = live->GetFile(0);
            return std::format("{} {:08X}{}{} {}", RE::FormTypeToString(live->GetFormType()), id,
                name && *name ? std::format(" \"{}\"", name) : "",
                editorID && *editorID ? std::format(" ({})", editorID) : "",
                live->IsDynamicForm() ? "created at runtime"s
                : file              ? std::format("from {}", file->GetFilename())
                                    : "from an unknown file"s);
        }

        std::string DescribeCell(const RE::TESObjectCELL* cell)
        {
            auto out = Describe(cell);
            if (cell->IsExteriorCell()) {
                const auto& data = cell->GetRuntimeData();
                if (const auto* world = Live(data.worldSpace)) {
                    const char* editorID = world->GetFormEditorID();
                    out += std::format(", in worldspace {:08X}{}", world->GetFormID(),
                        editorID && *editorID ? std::format(" ({})", editorID) : "");
                }
                if (const auto* exterior = data.cellData.exterior) {
                    out += std::format(" at grid ({}, {})", exterior->cellX, exterior->cellY);
                }
            }
            return out;
        }

        // A placed leveled actor gets a base created at runtime; this names what it was made from.
        std::string Origin(const RE::TESObjectREFR* reference)
        {
            const auto* leveled = reference->extraList.GetByType<RE::ExtraLeveledCreature>();
            if (!leveled) {
                return {};
            }
            return std::format("; leveled actor, original base {}, template {}", Describe(leveled->originalBase),
                Describe(leveled->templateBase));
        }

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
                    spdlog::info("  #{}: {} with base {}{}; cell {}", count,
                        RE::FormTypeToString(reference->GetFormType()), Describe(base), Origin(reference),
                        DescribeCell(cell));
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
