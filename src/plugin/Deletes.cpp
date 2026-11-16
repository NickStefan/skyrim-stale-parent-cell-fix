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
            std::uintptr_t cell = 0;  // attached, watched removals: the cell object
            bool inCellSet = false;   // attached: whether it was in the new cell's set right after
            bool parentKept = false;  // detached: whether parentCell still pointed at the cell after
        };

        Ring<Record, 8192> deletes;
        Ring<Record, 4096> released;  // formID set to 0
        Ring<Record, 1024> attached;  // a formID 0 reference given a non-null parentCell
        Ring<Record, 1024> detached;  // a formID 0 reference removed from a cell's reference set
        bool installed = false;
        bool releaseHooked = false;
        bool attachHooked = false;
        bool detachHooked = false;

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
            record.cell = 0;
            record.inCellSet = false;
            record.parentKept = false;
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

        // A formID 0 reference just put into a cell's set, checked until it leaves the set or the cell.
        struct Watch
        {
            std::uintptr_t reference;
            std::uintptr_t cell;
            std::uint32_t cellFormID;
            std::uint64_t attachedAt;
            std::uint64_t lastIn;
            Unlink::CellSetState lastState;
        };
        std::mutex watchLock;
        std::vector<Watch> watches;
        std::atomic<bool> watching = false;
        std::atomic<std::uint64_t> lastCheck = 0;
        constexpr std::size_t WatchLimit = 64;
        constexpr std::uint64_t CheckInterval = 20;  // ms

        // Every RemoveReference on a watched cell, so a lost reference shows what else left with it.
        Ring<Record, 512> watchedRemovals;
        std::atomic<std::uint64_t> lostCount = 0;

        void StartWatch(std::uintptr_t reference, std::uintptr_t cell)
        {
            const auto header = Forms::ReadHeader(cell);
            const auto now = GetTickCount64();
            std::scoped_lock guard{ watchLock };
            std::erase_if(watches, [&](const Watch& watch) { return watch.reference == reference; });
            if (watches.size() >= WatchLimit) {
                watches.erase(watches.begin());
            }
            watches.push_back({ reference, cell, header ? header->formID : 0, now, now,
                Unlink::ReadCellSet(cell, reference) });
            watching = true;
        }

        bool WatchedCell(std::uintptr_t cell)
        {
            std::scoped_lock guard{ watchLock };
            return std::ranges::any_of(watches, [&](const Watch& watch) { return watch.cell == cell; });
        }

        void ReportLost(const Watch& watch, const Unlink::CellSetState& now, std::uint64_t time)
        {
            std::string removals;
            watchedRemovals.Scan([&](const Record& record) {
                if (record.cell == watch.cell && record.time >= watch.lastIn) {
                    removals += std::format("\n      {:08X} (0x{:X}) {} ms ago on {}", record.formID, record.address,
                        time - record.time, Report::ThreadName(record.thread));
                }
                return true;
            });
            Stack stack;
            stack.Capture();
            spdlog::warn("formID 0 reference 0x{:X} left the reference list of cell {:08X} (0x{:X}) without "
                         "RemoveReference: still in it {} ms ago, gone now (put in {} ms ago); parentCell still "
                         "points at the cell. List then: slots 0x{:X}, capacity {}, free {}; now: slots 0x{:X}, "
                         "capacity {}, free {} (#{})",
                watch.reference, watch.cellFormID, watch.cell, time - watch.lastIn, time - watch.attachedAt,
                watch.lastState.entries, watch.lastState.capacity, watch.lastState.free, now.entries, now.capacity,
                now.free, ++lostCount);
            spdlog::info("  RemoveReference calls on that cell since it was last seen in the list:{}",
                removals.empty() ? std::string{ " none" } : removals);
            spdlog::info("  noticed on {}; stack:{}", Report::ThreadName(GetCurrentThreadId()),
                Report::StackTrace(stack.View()));
            Report::Notice("lost", "StaleParentCellFix: caught a reference dropping out of its cell. Please send "
                                   "StaleParentCellFix.log.");
        }

        void CheckWatches(bool force = false)
        {
            if (!watching) {
                return;
            }
            const auto time = GetTickCount64();
            auto previous = lastCheck.load();
            if (!force && (time - previous < CheckInterval || !lastCheck.compare_exchange_strong(previous, time))) {
                return;
            }
            std::vector<std::pair<Watch, Unlink::CellSetState>> lost;
            {
                std::scoped_lock guard{ watchLock };
                std::erase_if(watches, [&](Watch& watch) {
                    const auto parent =
                        Memory::Read<std::uintptr_t>(watch.reference + Forms::Offset::RefParentCell).value_or(0);
                    if (parent != watch.cell) {
                        return true;  // moved or unlinked: no longer the state that goes stale
                    }
                    const auto state = Unlink::ReadCellSet(watch.cell, watch.reference);
                    if (state.contains) {
                        watch.lastIn = time;
                        watch.lastState = state;
                        return false;
                    }
                    lost.emplace_back(watch, state);
                    return true;
                });
                watching = !watches.empty();
            }
            for (const auto& [watch, state] : lost) {
                ReportLost(watch, state, time);
            }
        }

        // ID 19818: TESObjectREFR::SetParentCell (vfunc 0x98; actors override it with 37304).
        using SetParentCellFn = void (*)(RE::TESObjectREFR*, RE::TESObjectCELL*);
        SetParentCellFn setParentCell = nullptr;

        void SetParentCell(RE::TESObjectREFR* reference, RE::TESObjectCELL* cell)
        {
            setParentCell(reference, cell);
            CheckWatches();
            if (!reference || !cell || reference->formID != 0) {
                return;
            }
            const auto address = reinterpret_cast<std::uintptr_t>(reference);
            const auto cellAddress = reinterpret_cast<std::uintptr_t>(cell);
            auto& record = Remember(attached, address);
            record.cell = cellAddress;
            record.inCellSet = Unlink::InCellSet(cellAddress, address);
            if (record.inCellSet) {
                StartWatch(address, cellAddress);
            }
        }

        // ID 19032: TESObjectCELL::RemoveReference. Its entry is `if (!reference) return;`, which
        // the hook does itself.
        using RemoveReferenceFn = void (*)(RE::TESObjectCELL*, RE::TESObjectREFR*);
        RemoveReferenceFn removeReference = nullptr;

        void RemoveReference(RE::TESObjectCELL* cell, RE::TESObjectREFR* reference)
        {
            if (!reference) {
                return;
            }
            const bool zero = reference->formID == 0;
            removeReference(cell, reference);
            if (watching && cell && WatchedCell(reinterpret_cast<std::uintptr_t>(cell))) {
                Remember(watchedRemovals, reinterpret_cast<std::uintptr_t>(reference)).cell =
                    reinterpret_cast<std::uintptr_t>(cell);
            }
            CheckWatches();
            if (!zero || !cell) {
                return;
            }
            const auto address = reinterpret_cast<std::uintptr_t>(reference);
            auto& record = Remember(detached, address);
            record.cellFormID = cell->formID;
            record.parentKept =
                Memory::Read<std::uintptr_t>(address + Forms::Offset::RefParentCell).value_or(0) ==
                reinterpret_cast<std::uintptr_t>(cell);
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

        // The newest few records of this reference in `ring`; false if there were none.
        bool History(std::string& out, const auto& ring, std::uintptr_t reference, auto describe)
        {
            std::vector<Record> found;
            ring.Scan([&](const Record& record) {
                if (record.address == reference) {
                    found.push_back(record);
                }
                return found.size() < 4;
            });
            for (const auto& record : found) {
                out += "\n    " + Line(describe(record), record);
            }
            return !found.empty();
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
        removeReference = Detour::EntryPastGuard("TESObjectCELL::RemoveReference", REL::ID(19032),
            std::array<std::uint8_t, 9>{ 0x48, 0x85, 0xD2, 0x0F, 0x84, 0x9D, 0x03, 0x00, 0x00 }, &RemoveReference);
        detachHooked = removeReference != nullptr;
        installed = true;
        spdlog::info("tracking who deletes references{}{}{}",
            releaseHooked ? ", who sets their formID to 0" : "; SetFormID not hooked",
            attachHooked ? ", who gives a formID 0 reference a cell" : "; SetParentCell not hooked",
            detachHooked ? ", and who removes one from a cell" : "; RemoveReference not hooked");
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
        const auto header = Forms::ReadHeader(reference);
        const bool zero = header && header->formID == 0;
        if (attachHooked) {
            const bool any = History(out, attached, reference, [](const Record& record) {
                return std::format("given parentCell 0x{:X} {}", record.cell,
                    record.inCellSet ? "(and in that cell's reference list)" : "but NOT in that cell's reference list");
            });
            if (!any && zero) {
                out += "\n    no SetParentCell recorded while it had formID 0";
            }
        }
        if (detachHooked) {
            const bool any = History(out, detached, reference, [](const Record& record) {
                return record.parentKept ? "removed from the cell's reference list, parentCell LEFT pointing at it"
                                         : "removed from the cell's reference list, parentCell changed";
            });
            if (!any && zero) {
                out += "\n    no RemoveReference recorded while it had formID 0";
            }
        }
        return out;
    }

    void Check()
    {
        CheckWatches(true);
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
