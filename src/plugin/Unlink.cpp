#include "plugin/Unlink.h"

#include "plugin/Deletes.h"
#include "plugin/Forms.h"
#include "plugin/Memory.h"

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
            std::uintptr_t address;
            std::uint32_t formID;
            std::uint32_t baseFormID;
            std::uint32_t flags;
            const char* list;
        };

        // Filled under the lock and logged after it is released: formatting call stacks is slow.
        thread_local std::vector<Cleared> cleared;

        void Unlink(RE::TESObjectREFR* reference, std::uintptr_t cell, const char* list)
        {
            if (!reference) {
                return;
            }
            const auto address = reinterpret_cast<std::uintptr_t>(reference);
            auto* parentCell = reinterpret_cast<std::uintptr_t*>(address + Forms::Offset::RefParentCell);
            if (*parentCell != cell) {
                return;
            }
            // Live persistent references are left stale, as the game leaves them: what reads their
            // cell while it is unloaded is unknown (quest markers, saving), and every crash so far
            // was on a formID 0 or deleted leftover.
            const auto header = Forms::ReadHeader(address);
            if (!header || (header->formID != 0 && !(header->flags & DeletedFlag))) {
                return;
            }
            *parentCell = 0;
            std::uint32_t baseFormID = 0;
            if (const auto base = Memory::Read<std::uintptr_t>(address + Forms::Offset::RefBaseObject)) {
                if (const auto baseHeader = Forms::ReadHeader(*base)) {
                    baseFormID = baseHeader->formID;
                }
            }
            cleared.push_back({ address, header->formID, baseFormID, header->flags, list });
        }

        void Log(std::uint32_t cellFormID)
        {
            for (const auto& entry : cleared) {
                spdlog::info("cell {:08X} destroyed: cleared stale parentCell of persistent reference 0x{:X} (formID "
                             "{:08X}, base {:08X}, flags {:08X}, {} list) (#{})",
                    cellFormID, entry.address, entry.formID, entry.baseFormID, entry.flags, entry.list, ++unlinked);
                if (const auto history = Deletes::Describe(entry.address); !history.empty()) {
                    spdlog::info("  0x{:X} {}", entry.address, history);
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

        void Scan(RE::TESWorldSpace* world, std::uintptr_t cell)
        {
            for (auto& reference : world->mobilePersistentRefs) {
                Unlink(reference.get(), cell, "mobile");
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
                    Unlink(entry.references->data[i], cell, "fixed");
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
                Scan(reinterpret_cast<RE::TESWorldSpace*>(worldSpace), address);
            } else if (auto* data = RE::TESDataHandler::GetSingleton()) {
                for (auto* world : data->GetFormArray<RE::TESWorldSpace>()) {
                    if (world) {
                        Scan(world, address);
                    }
                }
            }
            lock->Unlock();
            const auto elapsed = Microseconds() - start;
            Log(cellFormID);
            auto previous = slowest.load();
            while (elapsed > previous && !slowest.compare_exchange_weak(previous, elapsed)) {
            }
            if (elapsed > previous) {
                spdlog::info("slowest check so far: {} us for cell {:08X} ({})", elapsed, cellFormID,
                    Forms::IsLiveWorldSpace(worldSpace) ? "its worldspace" : "all worldspaces");
            }
        }

        using Deleter = void* (*)(void*, std::uint32_t);
        Deleter cellDelete = nullptr;

        void* CellDelete(void* self, std::uint32_t flags)
        {
            BeforeDestroy(static_cast<RE::TESObjectCELL*>(self));
            return cellDelete(self, flags);
        }
    }

    void Install()
    {
        lock.emplace();
        REL::Relocation<std::uintptr_t> cell{ RE::VTABLE_TESObjectCELL[0] };
        cellDelete = reinterpret_cast<Deleter>(cell.write_vfunc(0, &CellDelete));
        spdlog::info("unlinking persistent references from exterior cells before they are destroyed");
    }
}
