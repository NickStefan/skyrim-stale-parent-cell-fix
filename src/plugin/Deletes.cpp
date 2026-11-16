#include "plugin/Deletes.h"

#include "plugin/Detour.h"
#include "plugin/Forms.h"
#include "plugin/Memory.h"
#include "plugin/Report.h"
#include "plugin/Ring.h"
#include "plugin/Unlink.h"

#include <mutex>
#include <unordered_map>

namespace StaleParentCellFix::Deletes
{
    namespace
    {
        struct Record
        {
            std::uintptr_t address;
            std::uint64_t time;
            std::uint32_t formID;
            std::uint32_t baseFormID;
            std::uint32_t flags;  // before the deleted flag was set
            std::uint32_t cellFormID;
            std::uint32_t thread;
            Stack stack;
            bool inCellSet = false;  // attached: whether it was in the new cell's set right after
        };

        Ring<Record, 8192> deletes;
        Ring<Record, 4096> released;  // formID set to 0
        Ring<Record, 1024> attached;  // a formID 0 reference given a non-null parentCell
        bool installed = false;
        bool releaseHooked = false;
        bool attachHooked = false;

        // Copies taken at a reference's first formID 0 sighting, so they outlive the rings.
        struct Pinned
        {
            std::optional<Record> deleted;
            std::optional<Record> released;
        };
        std::mutex pinLock;
        std::unordered_map<std::uintptr_t, Pinned> pinned;
        constexpr std::size_t PinLimit = 4096;

        std::uint32_t FormIDAt(std::uintptr_t pointerAddress)
        {
            const auto form = Memory::Read<std::uintptr_t>(pointerAddress);
            if (!form || !*form) {
                return 0;
            }
            const auto header = Forms::ReadHeader(*form);
            return header ? header->formID : 0;
        }

        template <class R>
        Record& Remember(R& ring, std::uintptr_t address)
        {
            auto& record = ring.Next();
            record.address = address;
            record.time = GetTickCount64();
            const auto header = Forms::ReadHeader(address);
            record.formID = header ? header->formID : 0;
            record.flags = header ? header->flags : 0;
            record.baseFormID = FormIDAt(address + Forms::Offset::RefBaseObject);
            record.cellFormID = FormIDAt(address + Forms::Offset::RefParentCell);
            record.thread = GetCurrentThreadId();
            record.stack.Capture();
            return record;
        }

        using SetDeleteFn = void (*)(RE::TESObjectREFR*, bool);

        // One per vtable: each keeps the function its own slot held.
        template <std::size_t Slot>
        struct Hook
        {
            static inline SetDeleteFn original = nullptr;

            static void SetDelete(RE::TESObjectREFR* self, bool set)
            {
                if (set && self) {
                    Remember(deletes, reinterpret_cast<std::uintptr_t>(self));
                }
                original(self, set);
            }
        };

        // ID 14666: TESForm::SetFormID (vfunc 0x38, shared by every reference type, and called
        // directly from three places). Setting 0 removes the form from the form map.
        using SetFormIDFn = void (*)(RE::TESForm*, std::uint32_t, bool);
        SetFormIDFn setFormID = nullptr;

        constexpr std::uint8_t FirstReferenceType = 0x3D;  // REFR, then ACHR and the projectiles
        constexpr std::uint8_t LastReferenceType = 0x46;   // PHZD

        void SetFormID(RE::TESForm* form, std::uint32_t id, bool updateFile)
        {
            if (id == 0 && form && form->formID != 0) {
                const auto type = std::to_underlying(form->formType.get());
                if (type >= FirstReferenceType && type <= LastReferenceType) {
                    Remember(released, reinterpret_cast<std::uintptr_t>(form));
                }
            }
            setFormID(form, id, updateFile);
        }

        // ID 19818: TESObjectREFR::SetParentCell (vfunc 0x98; actors override it with 37304).
        using SetParentCellFn = void (*)(RE::TESObjectREFR*, RE::TESObjectCELL*);
        SetParentCellFn setParentCell = nullptr;

        void SetParentCell(RE::TESObjectREFR* reference, RE::TESObjectCELL* cell)
        {
            setParentCell(reference, cell);
            if (!reference || !cell || reference->formID != 0) {
                return;
            }
            const auto address = reinterpret_cast<std::uintptr_t>(reference);
            auto& record = Remember(attached, address);
            record.inCellSet = Unlink::InCellSet(reinterpret_cast<std::uintptr_t>(cell), address);
        }

        std::optional<Record> Find(const auto& ring, std::uintptr_t reference)
        {
            std::optional<Record> found;
            ring.Scan([&](const Record& record) {
                if (record.address == reference) {
                    found = record;
                    return false;
                }
                return true;
            });
            return found;
        }

        std::string Line(std::string_view what, const Record& record)
        {
            return std::format("{} {} ms ago on {} (formID {:08X}, base {:08X}, flags {:08X}, cell {:08X}); stack "
                               "then:{}",
                what, GetTickCount64() - record.time, Report::ThreadName(record.thread), record.formID,
                record.baseFormID, record.flags, record.cellFormID, Report::StackTrace(record.stack.View()));
        }

        template <std::size_t Slot>
        void Write(REL::VariantID vtable)
        {
            REL::Relocation<std::uintptr_t> table{ vtable };
            Hook<Slot>::original = reinterpret_cast<SetDeleteFn>(table.write_vfunc(0x23, &Hook<Slot>::SetDelete));
        }
    }

    void Install()
    {
        Write<0>(RE::VTABLE_TESObjectREFR[0]);
        Write<1>(RE::VTABLE_Actor[0]);
        Write<2>(RE::VTABLE_Character[0]);
        setFormID = Detour::Entry("TESForm::SetFormID", REL::ID(14666),
            std::array<std::uint8_t, 7>{ 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54 }, &SetFormID);
        releaseHooked = setFormID != nullptr;
        setParentCell = Detour::Entry("TESObjectREFR::SetParentCell", REL::ID(19818),
            std::array<std::uint8_t, 10>{ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 }, &SetParentCell);
        attachHooked = setParentCell != nullptr;
        installed = true;
        spdlog::info("tracking who deletes references{}{}",
            releaseHooked ? ", who sets their formID to 0" : "; SetFormID not hooked",
            attachHooked ? ", and who gives a formID 0 reference a cell" : "; SetParentCell not hooked");
    }

    std::string Describe(std::uintptr_t reference)
    {
        if (!installed) {
            return {};
        }
        Pinned pin;
        {
            std::scoped_lock guard{ pinLock };
            if (const auto it = pinned.find(reference); it != pinned.end()) {
                pin = it->second;
            }
        }
        std::string out;
        if (auto found = Find(deletes, reference); found || (found = pin.deleted)) {
            out = Line("deleted", *found);
        } else {
            out = "no deletion recorded this session (deleted before this game was loaded, by a path that bypasses "
                  "SetDelete, or too long ago)";
        }
        if (releaseHooked) {
            if (auto found = Find(released, reference); found || (found = pin.released)) {
                out += "\n    " + Line("formID set to 0", *found);
            } else if (const auto header = Forms::ReadHeader(reference); header && header->formID == 0) {
                out += "\n    formID 0 but no SetFormID(0) recorded this session (before this game was loaded, "
                       "written directly, or too long ago)";
            }
        }
        if (attachHooked) {
            std::vector<Record> found;
            attached.Scan([&](const Record& record) {
                if (record.address == reference) {
                    found.push_back(record);
                }
                return found.size() < 4;
            });
            for (const auto& record : found) {
                out += "\n    " + Line(record.inCellSet ? "given parentCell (and in that cell's reference list)"
                                                         : "given parentCell but NOT in that cell's reference list",
                                       record);
            }
            if (found.empty()) {
                if (const auto header = Forms::ReadHeader(reference); header && header->formID == 0) {
                    out += "\n    no SetParentCell recorded while it had formID 0";
                }
            }
        }
        return out;
    }

    void Pin(std::uintptr_t reference)
    {
        if (!installed) {
            return;
        }
        Pinned pin{ Find(deletes, reference), releaseHooked ? Find(released, reference) : std::nullopt };
        if (!pin.deleted && !pin.released) {
            return;
        }
        std::scoped_lock guard{ pinLock };
        if (pinned.size() > PinLimit) {
            pinned.clear();
        }
        pinned.insert_or_assign(reference, std::move(pin));
    }
}
