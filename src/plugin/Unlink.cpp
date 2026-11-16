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

        inline constexpr std::uint32_t DeletedFlag = 0x20;

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
        };
        thread_local std::vector<Sighting> sightings;
        std::unordered_set<std::uintptr_t> sighted;  // only touched while holding the game's lock

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
            if (!header || header->formID != 0 || !sighted.insert(address).second) {
                return;
            }
            sightings.push_back({ address, BaseFormID(address), header->flags,
                Memory::Read<std::array<float, 3>>(address + Forms::Offset::RefPosition).value_or(std::array<float, 3>{}),
                Memory::Read<std::uintptr_t>(address + Forms::Offset::RefParentCell).value_or(0), list });
        }

        void LogSightings()
        {
            for (const auto& s : sightings) {
                std::string parent;
                if (s.parent == 0) {
                    parent = "null";
                } else if (Forms::IsLiveCell(s.parent)) {
                    const auto header = Forms::ReadHeader(s.parent);
                    parent = std::format("live cell {:08X}", header ? header->formID : 0);
                } else if (auto freed = Frees::Brief(s.parent); !freed.empty()) {
                    parent = std::move(freed);
                } else {
                    parent = "not a live cell";
                }
                spdlog::warn("formID 0 persistent reference 0x{:X} first seen ({} list): base {:08X}, flags {:08X}, "
                             "position ({:.0f}, {:.0f}, {:.0f}); parentCell 0x{:X}: {}",
                    s.address, s.list, s.baseFormID, s.flags, s.position[0], s.position[1], s.position[2], s.parent,
                    parent);
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

        // References left pointing at a destroyed cell, for the guards to recognise a stale read.
        // Keyed by reference; an entry only counts while the reference still holds that pointer.
        constexpr std::size_t StaleLimit = 200000;
        thread_local std::vector<std::pair<std::uintptr_t, Stale>> leftStale;
        std::mutex staleMutex;
        std::unordered_map<std::uintptr_t, Stale> stale;
        std::atomic<std::uint64_t> staleRead{ 0 };

        void Unlink(RE::TESObjectREFR* reference, std::uintptr_t cell, std::uint32_t cellFormID, const char* list)
        {
            if (!reference) {
                return;
            }
            const auto address = reinterpret_cast<std::uintptr_t>(reference);
            Sight(address, list);
            auto* parentCell = reinterpret_cast<std::uintptr_t*>(address + Forms::Offset::RefParentCell);
            if (*parentCell != cell) {
                return;
            }
            // Live persistent references are left stale, as the game leaves them: what reads their
            // cell while it is unloaded is unknown (quest markers, saving), and every crash so far
            // was on a formID 0 or deleted leftover.
            const auto header = Forms::ReadHeader(address);
            if (!header) {
                return;
            }
            std::uint32_t baseFormID = 0;
            if (const auto base = Memory::Read<std::uintptr_t>(address + Forms::Offset::RefBaseObject)) {
                if (const auto baseHeader = Forms::ReadHeader(*base)) {
                    baseFormID = baseHeader->formID;
                }
            }
            const bool leftover = header->formID == 0 || (header->flags & DeletedFlag);
            if (!clearing || !leftover) {
                leftStale.push_back({ address, { cell, cellFormID, GetTickCount64(), header->formID, baseFormID,
                                                   header->flags } });
                return;
            }
            *parentCell = 0;
            cleared.push_back({ RE::NiPointer{ reference }, address, header->formID, baseFormID, header->flags, list });
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
                spdlog::info("cell {:08X} destroyed: cleared stale parentCell of persistent reference 0x{:X} (formID "
                             "{:08X}, base {:08X}, flags {:08X}, {} list) (#{})",
                    cellFormID, entry.address, entry.formID, entry.baseFormID, entry.flags, entry.list, ++unlinked);
                spdlog::info("  0x{:X} {}", entry.address, Holders(entry.reference.get()));
                if (const auto history = Deletes::Describe(entry.address); !history.empty()) {
                    spdlog::info("  0x{:X} {}", entry.address, history);
                }
                if (entry.formID == 0) {
                    Report::Notice("cleared", "StaleParentCellFix: prevented a crash on a formID 0 reference. "
                                              "Please send StaleParentCellFix.log.");
                }
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

        void Scan(RE::TESWorldSpace* world, std::uintptr_t cell, std::uint32_t cellFormID)
        {
            for (auto& reference : world->mobilePersistentRefs) {
                Unlink(reference.get(), cell, cellFormID, "mobile");
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
                    Unlink(entry.references->data[i], cell, cellFormID, "fixed");
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
            // A purged exterior cell has usually lost its worldSpace pointer by the time it is
            // destroyed, so without one every worldspace is checked.
            const auto worldSpace = Memory::Read<std::uintptr_t>(address + Forms::Offset::CellWorldSpace).value_or(0);
            if (Forms::IsLiveWorldSpace(worldSpace)) {
                Scan(reinterpret_cast<RE::TESWorldSpace*>(worldSpace), address, cellFormID);
            } else if (auto* data = RE::TESDataHandler::GetSingleton()) {
                for (auto* world : data->GetFormArray<RE::TESWorldSpace>()) {
                    if (world) {
                        Scan(world, address, cellFormID);
                    }
                }
            }
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

        // After the game's destructor ran: which of the references that pointed at the cell still
        // do. Only those are stale; the rest were re-pointed by the destruction itself.
        void AfterDestroy(std::uint32_t cellFormID)
        {
            if (leftStale.empty()) {
                return;
            }
            std::uint64_t still = 0, nulled = 0, elsewhere = 0;
            std::scoped_lock guard{ staleMutex };
            if (stale.size() + leftStale.size() > StaleLimit) {
                spdlog::info("stale reads: forgetting {} remembered stale references", stale.size());
                stale.clear();
            }
            for (const auto& [reference, entry] : leftStale) {
                const auto parent = Memory::Read<std::uintptr_t>(reference + Forms::Offset::RefParentCell).value_or(0);
                if (entry.formID == 0 || (entry.flags & DeletedFlag)) {
                    spdlog::info("cell {:08X} destroyed: leftover 0x{:X} (formID {:08X}, base {:08X}, flags {:08X}) "
                                 "afterwards {}",
                        cellFormID, reference, entry.formID, entry.baseFormID, entry.flags,
                        parent == entry.cell ? "still points at it (stale)"
                        : parent == 0        ? "has parentCell null"
                                             : "points at another cell");
                }
                if (parent == entry.cell) {
                    ++still;
                    stale.insert_or_assign(reference, entry);
                } else if (parent == 0) {
                    ++nulled;
                } else {
                    ++elsewhere;
                }
            }
            spdlog::info("cell {:08X} destroyed: of {} persistent reference(s) pointing at it, {} still do afterwards "
                         "(stale), {} were set to null, {} to another cell; stale pointers remembered {}, read {}",
                cellFormID, leftStale.size(), still, nulled, elsewhere, stale.size(), staleRead.load());
            leftStale.clear();
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
        spdlog::info(clear ? "unlinking deleted persistent references from exterior cells before they are destroyed; "
                             "remembering the rest for stale-read reports"
                           : "fix off: only remembering persistent references left pointing at destroyed cells, for "
                             "stale-read reports");
    }

    std::optional<Stale> FindStale(std::uintptr_t reference, std::uintptr_t parentCell, bool forget)
    {
        std::scoped_lock guard{ staleMutex };
        const auto it = stale.find(reference);
        if (it == stale.end()) {
            return std::nullopt;
        }
        if (it->second.cell != parentCell) {
            stale.erase(it);
            return std::nullopt;
        }
        const auto found = it->second;
        ++staleRead;
        if (forget) {
            stale.erase(it);
        }
        return found;
    }
}
