#include "plugin/Unlink.h"

#include <unordered_set>

#include "plugin/Deletes.h"
#include "plugin/Forms.h"
#include "plugin/Frees.h"
#include "plugin/Memory.h"
#include "plugin/Report.h"

namespace StaleParentCellFix::Unlink
{
    namespace
    {
        // ID 401119: the recursive spin lock (owning thread, count) the game holds around the
        // worldspace persistent reference lists, e.g. in the loader (20532) and clear (20522).
        class PersistentRefsLock
        {
        public:
            PersistentRefsLock()
            {
                REL::Relocation<std::uint32_t*> base{ REL::ID(401119) };
                owner = base.get();
                count = base.get() + 1;
            }

            // Bounded: the caller may hold locks the loader thread is waiting for while it holds
            // this one, so give up rather than risk a deadlock.
            bool TryLock()
            {
                const auto self = static_cast<std::uint32_t>(GetCurrentThreadId());
                if (std::atomic_ref{ *owner }.load() == self) {
                    std::atomic_ref{ *count }.fetch_add(1);
                    return true;
                }
                for (int attempt = 0; attempt < 20000; ++attempt) {
                    std::uint32_t expected = 0;
                    if (std::atomic_ref{ *count }.compare_exchange_strong(expected, 1)) {
                        std::atomic_ref{ *owner }.store(self);
                        return true;
                    }
                    if (attempt < 1000) {
                        YieldProcessor();
                    } else {
                        Sleep(0);
                    }
                }
                return false;
            }

            void Unlock()
            {
                auto counter = std::atomic_ref{ *count };
                if (counter.load() == 1) {
                    std::atomic_ref{ *owner }.store(0);
                }
                counter.fetch_sub(1);
            }

        private:
            std::uint32_t* owner;
            std::uint32_t* count;
        };

        std::optional<PersistentRefsLock> lock;
        std::atomic<std::uint64_t> unlinked{ 0 };
        std::atomic<std::uint64_t> skipped{ 0 };
        std::atomic<std::int64_t> slowest{ 500 };

        std::int64_t Microseconds()
        {
            static const auto frequency = [] {
                LARGE_INTEGER value;
                QueryPerformanceFrequency(&value);
                return value.QuadPart;
            }();
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            return now.QuadPart * 1'000'000 / frequency;
        }

        struct Cleared
        {
            RE::NiPointer<RE::TESObjectREFR> reference;  // keeps it alive until it is logged
            std::uintptr_t address;
            std::uint32_t formID;
            std::uint32_t baseFormID;
            std::uint32_t flags;
            const char* list;
        };

        // Filled under the lock and logged after it is released: formatting call stacks is slow.
        thread_local std::vector<Cleared> cleared;

        bool clearing = true;

        // formID 0 references in the persistent lists, reported the first time a scan sees them:
        // every proven stale reference so far had formID 0.
        struct Sighting
        {
            std::uintptr_t address;
            std::uint32_t baseFormID;
            std::uint32_t flags;
            std::array<float, 3> position;
            std::uintptr_t parent;
            const char* list;
            std::uint32_t lastFormID;  // 0 if never seen with one
            std::uint64_t lastSeen;
        };
        thread_local std::vector<Sighting> sightings;
        // Only touched while holding the game's lock.
        std::unordered_set<std::uintptr_t> sighted;
        // The formID each deleted persistent reference had when a scan last saw it, so a formID 0
        // sighting can say what the reference was and narrow down when its ID was cleared.
        struct Known
        {
            std::uint32_t formID;
            std::uint64_t time;
        };
        std::unordered_map<std::uintptr_t, Known> knownDeleted;
        constexpr std::size_t KnownLimit = 100000;
        inline constexpr std::uint32_t DeletedFlag = 0x20;
        std::uint64_t firstScan = 0;

        std::unordered_set<std::uintptr_t> CellSet(std::uintptr_t cell);

        std::uint32_t BaseFormID(std::uintptr_t reference)
        {
            if (const auto base = Memory::Read<std::uintptr_t>(reference + Forms::Offset::RefBaseObject)) {
                if (const auto header = Forms::ReadHeader(*base)) {
                    return header->formID;
                }
            }
            return 0;
        }

        void Sight(std::uintptr_t address, const char* list)
        {
            const auto header = Forms::ReadHeader(address);
            if (!header) {
                return;
            }
            if (header->formID != 0) {
                if (header->flags & DeletedFlag) {
                    if (knownDeleted.size() > KnownLimit) {
                        knownDeleted.clear();
                    }
                    knownDeleted.insert_or_assign(address, Known{ header->formID, GetTickCount64() });
                } else if (!knownDeleted.empty()) {
                    knownDeleted.erase(address);  // the address now holds another, live reference
                }
                return;
            }
            if (!sighted.insert(address).second) {
                return;
            }
            const auto known = knownDeleted.find(address);
            sightings.push_back({ address, BaseFormID(address), header->flags,
                Memory::Read<std::array<float, 3>>(address + Forms::Offset::RefPosition).value_or(std::array<float, 3>{}),
                Memory::Read<std::uintptr_t>(address + Forms::Offset::RefParentCell).value_or(0), list,
                known != knownDeleted.end() ? known->second.formID : 0,
                known != knownDeleted.end() ? known->second.time : 0 });
        }

        void LogSightings()
        {
            for (const auto& s : sightings) {
                std::string parent;
                if (s.parent == 0) {
                    parent = "null";
                } else if (Forms::IsLiveCell(s.parent)) {
                    const auto header = Forms::ReadHeader(s.parent);
                    parent = std::format("live cell {:08X}, {} its own reference list", header ? header->formID : 0,
                        CellSet(s.parent).contains(s.address) ? "still in" : "NOT in");
                } else if (auto freed = Frees::Brief(s.parent); !freed.empty()) {
                    parent = std::move(freed);
                } else {
                    parent = "not a live cell";
                }
                spdlog::warn("formID 0 persistent reference 0x{:X} first seen ({} list): base {:08X}, flags {:08X}, "
                             "position ({:.0f}, {:.0f}, {:.0f}); parentCell 0x{:X}: {}",
                    s.address, s.list, s.baseFormID, s.flags, s.position[0], s.position[1], s.position[2], s.parent,
                    parent);
                const auto now = GetTickCount64();
                if (s.lastFormID != 0) {
                    spdlog::info("  0x{:X} was formID {:08X}, deleted, when last checked {} ms ago", s.address,
                        s.lastFormID, now - s.lastSeen);
                } else {
                    spdlog::info("  0x{:X} never seen with a formID while deleted; the first check this session was {} "
                                 "ms ago",
                        s.address, now - firstScan);
                }
                if (const auto history = Deletes::Describe(s.address); !history.empty()) {
                    spdlog::info("  0x{:X} {}", s.address, history);
                }
            }
            if (!sightings.empty()) {
                Report::Notice("sighting", "StaleParentCellFix: caught a formID 0 reference, the kind behind the "
                                           "crash. Please send StaleParentCellFix.log.");
            }
            sightings.clear();
        }

        // References still pointing at the cell that this left alone; the destructor should null them.
        struct Left
        {
            std::uintptr_t address;
            std::uintptr_t cell;
            std::uint32_t formID;
            std::uint32_t baseFormID;
            std::uint32_t flags;
            bool inCellSet;
        };
        thread_local std::vector<Left> left;
        std::uint64_t stayedStale = 0;

        // Persistent-list references pointing at the cell being destroyed, found by Scan.
        struct Pointing
        {
            RE::TESObjectREFR* reference;
            const char* list;
        };
        thread_local std::vector<Pointing> pointing;

        void Collect(RE::TESObjectREFR* reference, std::uintptr_t cell, const char* list)
        {
            if (!reference) {
                return;
            }
            const auto address = reinterpret_cast<std::uintptr_t>(reference);
            Sight(address, list);
            if (*reinterpret_cast<std::uintptr_t*>(address + Forms::Offset::RefParentCell) == cell) {
                pointing.push_back({ reference, list });
            }
        }

        // The cell's own reference set (a BSTSet at +0x90): the only list its destructor walks
        // (19410 -> 19458 -> 19468, which calls SetParentCell(nullptr) on each reference).
        inline constexpr std::uintptr_t CellSetCapacity = 0x94;
        inline constexpr std::uintptr_t CellSetEntries = 0xB0;

        struct CellSetEntry
        {
            std::uintptr_t reference;
            std::uintptr_t next;  // null when the slot is empty
        };
        static_assert(sizeof(CellSetEntry) == 0x10);

        std::unordered_set<std::uintptr_t> CellSet(std::uintptr_t cell)
        {
            std::unordered_set<std::uintptr_t> out;
            const auto capacity = Memory::Read<std::uint32_t>(cell + CellSetCapacity).value_or(0);
            const auto entries = Memory::Read<std::uintptr_t>(cell + CellSetEntries).value_or(0);
            if (!entries || capacity > (1u << 22)) {
                return out;
            }
            for (std::uint32_t slot = 0; slot < capacity; ++slot) {
                const auto entry = Memory::Read<CellSetEntry>(entries + slot * sizeof(CellSetEntry));
                if (!entry) {
                    break;
                }
                if (entry->next && entry->reference) {
                    out.insert(entry->reference);
                }
            }
            return out;
        }

        // The destructor nulls parentCell only on references in the cell's own set, so the ones
        // pointing at it from the persistent lists but missing from that set would stay stale.
        void Decide(std::uintptr_t cell)
        {
            if (pointing.empty()) {
                return;
            }
            std::ranges::sort(pointing, {}, &Pointing::reference);
            const auto [first, last] = std::ranges::unique(pointing, {}, &Pointing::reference);
            pointing.erase(first, last);
            const auto members = CellSet(cell);
            for (const auto& [reference, list] : pointing) {
                const auto address = reinterpret_cast<std::uintptr_t>(reference);
                const auto header = Forms::ReadHeader(address);
                if (!header) {
                    continue;
                }
                const auto baseFormID = BaseFormID(address);
                const bool inCellSet = members.contains(address);
                if (inCellSet || !clearing) {
                    left.push_back({ address, cell, header->formID, baseFormID, header->flags, inCellSet });
                    continue;
                }
                *reinterpret_cast<std::uintptr_t*>(address + Forms::Offset::RefParentCell) = 0;
                cleared.push_back({ RE::NiPointer{ reference }, address, header->formID, baseFormID, header->flags, list });
            }
            pointing.clear();
        }

        std::string_view LifeState(RE::ACTOR_LIFE_STATE state)
        {
            using enum RE::ACTOR_LIFE_STATE;
            switch (state) {
            case kAlive: return "alive";
            case kDying: return "dying";
            case kDead: return "dead";
            case kUnconcious: return "unconscious";
            case kReanimate: return "reanimated";
            case kRecycle: return "recycle";
            case kRestrained: return "restrained";
            case kEssentialDown: return "essential down";
            case kBleedout: return "bleedout";
            default: return "unknown";
            }
        }

        // What may be keeping a deleted reference in the persistent lists: forced persistence (set
        // while a quest alias or script holds it) and the quest aliases filled with it.
        std::string Holders(RE::TESObjectREFR* reference)
        {
            const bool forced = reference->inGameFormFlags.any(RE::TESForm::InGameFormFlag::kForcedPersistent);
            auto out = std::format("forced persistent {}", forced ? "yes" : "no");
            if (reference->GetFormType() == RE::FormType::ActorCharacter) {
                const auto state = static_cast<RE::Actor*>(reference)->AsActorState()->GetLifeState();
                out += std::format(", life state {} ({})", LifeState(state), std::to_underlying(state));
            }
            const auto* aliases = reference->extraList.GetByType<RE::ExtraAliasInstanceArray>();
            if (!aliases) {
                return out + ", no quest aliases";
            }
            RE::BSReadLockGuard guard{ aliases->lock };
            out += std::format(", {} quest alias(es):", aliases->aliases.size());
            for (const auto* instance : aliases->aliases) {
                if (!instance) {
                    continue;
                }
                const auto* quest = instance->quest;
                const auto questID = quest ? quest->GetFormID() : 0;
                const char* questName = quest ? quest->GetFormEditorID() : nullptr;
                const char* aliasName = instance->alias ? instance->alias->aliasName.c_str() : nullptr;
                out += std::format(" quest {:08X} {} alias \"{}\"{};", questID, questName && *questName ? questName : "?",
                    aliasName ? aliasName : "?", quest && quest->IsRunning() ? " (running)" : "");
            }
            return out;
        }

        void Log(std::uint32_t cellFormID)
        {
            for (const auto& entry : cleared) {
                spdlog::warn("cell {:08X} destroyed: cleared parentCell of persistent reference 0x{:X} (formID "
                             "{:08X}, base {:08X}, flags {:08X}, {} list): not in the cell's own reference list, so "
                             "the destructor would have left it pointing at the freed cell (#{})",
                    cellFormID, entry.address, entry.formID, entry.baseFormID, entry.flags, entry.list, ++unlinked);
                spdlog::info("  0x{:X} {}", entry.address, Holders(entry.reference.get()));
                if (const auto history = Deletes::Describe(entry.address); !history.empty()) {
                    spdlog::info("  0x{:X} {}", entry.address, history);
                }
                Report::Notice("cleared", "StaleParentCellFix: prevented a stale cell pointer, the kind behind the "
                                          "crash. Please send StaleParentCellFix.log.");
            }
            cleared.clear();
        }

        // CommonLib declares fixedPersistentRefMap's values as arrays stored in the entry; the game
        // stores a pointer to a separately allocated array (see its clear, 20522, and 19249).
        struct ReferenceArray
        {
            RE::TESObjectREFR** data;
            std::uint32_t capacity;
            std::uint32_t pad0C;
            std::uint32_t size;
        };
        static_assert(offsetof(ReferenceArray, size) == 0x10);

        struct FixedEntry
        {
            std::uint64_t cellID;
            ReferenceArray* references;
            FixedEntry* next;  // null when the slot is empty
        };
        static_assert(sizeof(FixedEntry) == 0x18);

        inline constexpr std::uintptr_t FixedMapCapacity = 0xB4;
        inline constexpr std::uintptr_t FixedMapEntries = 0xD0;

        void Scan(RE::TESWorldSpace* world, std::uintptr_t cell)
        {
            for (auto& reference : world->mobilePersistentRefs) {
                Collect(reference.get(), cell, "mobile");
            }
            const auto address = reinterpret_cast<std::uintptr_t>(world);
            const auto capacity = *reinterpret_cast<std::uint32_t*>(address + FixedMapCapacity);
            const auto* entries = *reinterpret_cast<FixedEntry**>(address + FixedMapEntries);
            if (!entries) {
                return;
            }
            for (std::uint32_t slot = 0; slot < capacity; ++slot) {
                const auto& entry = entries[slot];
                if (!entry.next || !entry.references || !entry.references->data) {
                    continue;
                }
                for (std::uint32_t i = 0; i < entry.references->size; ++i) {
                    Collect(entry.references->data[i], cell, "fixed");
                }
            }
        }

        void BeforeDestroy(RE::TESObjectCELL* cell)
        {
            const auto address = reinterpret_cast<std::uintptr_t>(cell);
            const auto header = Forms::ReadHeader(address);
            const auto cellFormID = header ? header->formID : 0;
            if (!lock->TryLock()) {
                spdlog::warn("could not take the persistent reference lock while cell {:08X} was destroyed; its "
                             "references were not checked (#{})",
                    cellFormID, ++skipped);
                return;
            }
            const auto start = Microseconds();
            if (firstScan == 0) {
                firstScan = GetTickCount64();
            }
            // A purged exterior cell has usually lost its worldSpace pointer by the time it is
            // destroyed, so without one every worldspace is checked.
            const auto worldSpace = Memory::Read<std::uintptr_t>(address + Forms::Offset::CellWorldSpace).value_or(0);
            if (Forms::IsLiveWorldSpace(worldSpace)) {
                Scan(reinterpret_cast<RE::TESWorldSpace*>(worldSpace), address);
            } else if (auto* data = RE::TESDataHandler::GetSingleton()) {
                for (auto* world : data->GetFormArray<RE::TESWorldSpace>()) {
                    if (world) {
                        Scan(world, address);
                    }
                }
            }
            Decide(address);
            lock->Unlock();
            const auto elapsed = Microseconds() - start;
            Log(cellFormID);
            LogSightings();

            auto previous = slowest.load();
            while (elapsed > previous && !slowest.compare_exchange_weak(previous, elapsed)) {
            }
            if (elapsed > previous) {
                spdlog::info("slowest check so far: {} us for cell {:08X} ({})", elapsed, cellFormID,
                    Forms::IsLiveWorldSpace(worldSpace) ? "its worldspace" : "all worldspaces");
            }
        }

        // The fix relies on the destructor nulling every reference in the cell's own set.
        void AfterDestroy(std::uint32_t cellFormID)
        {
            for (const auto& entry : left) {
                const auto parent =
                    Memory::Read<std::uintptr_t>(entry.address + Forms::Offset::RefParentCell).value_or(0);
                if (parent != entry.cell) {
                    if (!entry.inCellSet) {
                        spdlog::warn("cell {:08X} destroyed: persistent reference 0x{:X} (formID {:08X}) was not in "
                                     "the cell's own reference list, yet was re-pointed anyway",
                            cellFormID, entry.address, entry.formID);
                    }
                    continue;
                }
                spdlog::warn("cell {:08X} destroyed: persistent reference 0x{:X} (formID {:08X}, base {:08X}, flags "
                             "{:08X}) still points at it afterwards; {} (#{})",
                    cellFormID, entry.address, entry.formID, entry.baseFormID, entry.flags,
                    entry.inCellSet ? "it was in the cell's own reference list, so the destructor skipped it for "
                                      "another reason"
                                    : "it was not in the cell's own reference list (fix off)",
                    ++stayedStale);
                if (const auto history = Deletes::Describe(entry.address); !history.empty()) {
                    spdlog::info("  0x{:X} {}", entry.address, history);
                }
                Report::Notice("stayed stale", "StaleParentCellFix: a reference was left pointing at a destroyed "
                                               "cell. Please send StaleParentCellFix.log.");
            }
            left.clear();
        }

        using Deleter = void* (*)(void*, std::uint32_t);
        Deleter cellDelete = nullptr;

        void* CellDelete(void* self, std::uint32_t flags)
        {
            const auto header = Forms::ReadHeader(reinterpret_cast<std::uintptr_t>(self));
            BeforeDestroy(static_cast<RE::TESObjectCELL*>(self));
            const auto result = cellDelete(self, flags);
            AfterDestroy(header ? header->formID : 0);
            return result;
        }
    }

    void Install(bool clear)
    {
        clearing = clear;
        lock.emplace();
        REL::Relocation<std::uintptr_t> cell{ RE::VTABLE_TESObjectCELL[0] };
        cellDelete = reinterpret_cast<Deleter>(cell.write_vfunc(0, &CellDelete));
        spdlog::info(clear ? "before a cell is destroyed, unlinking persistent references that point at it but are "
                             "not in its own reference list; checking the rest afterwards"
                           : "fix off: only checking afterwards which persistent references still point at destroyed "
                             "cells");
    }
}
