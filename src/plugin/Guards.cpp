#include "plugin/Guards.h"

#include "plugin/Detour.h"
#include "plugin/Forms.h"
#include "plugin/Memory.h"
#include "plugin/Report.h"

namespace StaleParentCellFix::Guards
{
    namespace
    {
        using Kind = Forms::Kind;

        std::uintptr_t Address(const void* pointer)
        {
            return reinterpret_cast<std::uintptr_t>(pointer);
        }

        // ID 18365: does the reference's (current or editor) location equal this location or one
        // of its children. Called by the GetInCurrentLocAlias / GetIsEditorLocAlias style condition
        // functions (21656, 21654) while the story manager fills quest aliases.
        using IsChildFn = bool (*)(RE::BGSLocation*, RE::TESObjectREFR*, bool);
        IsChildFn isChild = nullptr;

        bool IsChild(RE::BGSLocation* location, RE::TESObjectREFR* reference, bool current)
        {
            if (reference) {
                if (const auto why = Forms::WhyDead(Address(reference)); !why.empty()) {
                    Report::Incident("BGSLocation::IsChild (ID 18365)", why,
                        { { "reference", Address(reference), Kind::Reference },
                            { "location", Address(location), Kind::Location } });
                    return false;
                }
            }
            return isChild(location, reference, current);
        }

        // ID 20539: TESWorldSpace::GetLocationFromCoordinates. Crash: 20539+0x73 with a garbage
        // worldspace taken from a freed parent cell.
        using WorldLocationAtFn = RE::BGSLocation* (*)(RE::TESWorldSpace*, const RE::NiPoint3*);
        WorldLocationAtFn worldLocationAt = nullptr;

        RE::BGSLocation* WorldLocationAt(RE::TESWorldSpace* world, const RE::NiPoint3* position)
        {
            if (!Forms::IsLiveWorldSpace(Address(world))) {
                Report::Incident("TESWorldSpace::GetLocationFromCoordinates (ID 20539)", "worldspace is not live",
                    { { "worldSpace", Address(world), Kind::WorldSpace } });
                return nullptr;
            }
            return worldLocationAt(world, position);
        }

        // ID 20498: TESWorldSpace::GetLocation. Crash: 20498+0x6, same garbage worldspace.
        using WorldLocationFn = RE::BGSLocation* (*)(RE::TESWorldSpace*);
        WorldLocationFn worldLocation = nullptr;

        RE::BGSLocation* WorldLocation(RE::TESWorldSpace* world)
        {
            if (!Forms::IsLiveWorldSpace(Address(world))) {
                Report::Incident("TESWorldSpace::GetLocation (ID 20498)", "worldspace is not live",
                    { { "worldSpace", Address(world), Kind::WorldSpace } });
                return nullptr;
            }
            return worldLocation(world);
        }

        // ID 18905: TESObjectCELL::GetLocation.
        using CellLocationFn = RE::BGSLocation* (*)(RE::TESObjectCELL*);
        CellLocationFn cellLocation = nullptr;

        RE::BGSLocation* CellLocation(RE::TESObjectCELL* cell)
        {
            if (!Forms::IsLiveCell(Address(cell))) {
                Report::Incident("TESObjectCELL::GetLocation (ID 18905)", "cell is not live",
                    { { "cell", Address(cell), Kind::Cell } });
                return nullptr;
            }
            return cellLocation(cell);
        }

        // ID 18988: TESObjectCELL::GetEncounterZone. Crash: 18988+0x15 with a freed cell whose
        // loadedData had been overwritten by UTF-16 text.
        using CellEncounterZoneFn = RE::BGSEncounterZone* (*)(RE::TESObjectCELL*);
        CellEncounterZoneFn cellEncounterZone = nullptr;

        RE::BGSEncounterZone* CellEncounterZone(RE::TESObjectCELL* cell)
        {
            if (!Forms::IsLiveCell(Address(cell))) {
                Report::Incident("TESObjectCELL::GetEncounterZone (ID 18988)", "cell is not live",
                    { { "cell", Address(cell), Kind::Cell } });
                return nullptr;
            }
            return cellEncounterZone(cell);
        }

        // ID 19411: TESObjectCELL::AddReference (cell loader and main thread). It first removes the
        // reference from its current parentCell (19032), which is where a freed parent cell gets
        // used; a dead parent is cut so that step is skipped.
        using AddReferenceFn = void (*)(RE::TESObjectCELL*, RE::TESObjectREFR*, std::uint64_t, std::uint64_t);
        AddReferenceFn addReference = nullptr;

        void AddReference(RE::TESObjectCELL* cell, RE::TESObjectREFR* reference, std::uint64_t arg3, std::uint64_t arg4)
        {
            const auto address = Address(reference);
            if (reference) {
                const auto header = Forms::ReadHeader(address);
                const auto parent = Memory::Read<std::uintptr_t>(address + Forms::Offset::RefParentCell);
                if (!header || !parent) {
                    Report::Incident("TESObjectCELL::AddReference (ID 19411)", "reference unreadable; not added",
                        { { "reference", address, Kind::Reference }, { "cell", Address(cell), Kind::Cell } });
                    return;
                }
                if (*parent != 0 && *parent != Address(cell) && !Forms::IsLiveCell(*parent)) {
                    Report::Incident("TESObjectCELL::AddReference (ID 19411)",
                        "reference's previous parentCell is not live; link cut before adding",
                        { { "reference", address, Kind::Reference }, { "cell", Address(cell), Kind::Cell } });
                    *reinterpret_cast<std::uintptr_t*>(address + Forms::Offset::RefParentCell) = 0;
                }
            }
            addReference(cell, reference, arg3, arg4);
        }

        template <class F>
        void Guard(F& original, std::string_view name, std::uint64_t id, std::initializer_list<std::uint8_t> prologue,
            F hook)
        {
            original = Detour::Entry(name, REL::ID(id), std::span{ prologue.begin(), prologue.size() }, hook);
        }
    }

    void Install()
    {
        Guard(isChild, "BGSLocation::IsChild", 18365, { 0x48, 0x89, 0x5C, 0x24, 0x08 }, &IsChild);
        Guard(worldLocationAt, "TESWorldSpace::GetLocationFromCoordinates", 20539, { 0x48, 0x89, 0x5C, 0x24, 0x10 },
            &WorldLocationAt);
        Guard(worldLocation, "TESWorldSpace::GetLocation", 20498, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 },
            &WorldLocation);
        Guard(cellLocation, "TESObjectCELL::GetLocation", 18905, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30 }, &CellLocation);
        Guard(cellEncounterZone, "TESObjectCELL::GetEncounterZone", 18988, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 },
            &CellEncounterZone);
        Guard(addReference, "TESObjectCELL::AddReference", 19411, { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54 },
            &AddReference);
    }
}
