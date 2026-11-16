#include "plugin/Events.h"

#include "plugin/Detour.h"
#include "plugin/Forms.h"
#include "plugin/Memory.h"
#include "plugin/Report.h"
#include "plugin/Ring.h"

namespace StaleParentCellFix::Events
{
    namespace
    {
        constexpr std::size_t PayloadWords = 4;
        constexpr std::size_t Shown = 4;

        struct Record
        {
            std::uint64_t time;
            std::uint32_t thread;
            std::uint32_t index;
            std::array<char, 4> type;
            std::array<std::uintptr_t, PayloadWords> payload;
            Stack stack;
        };

        Ring<Record, 256> changeLocation;
        bool installed = false;
        bool purge = false;
        std::atomic<std::uint64_t> purges{ 0 };

        void QueuePurge()
        {
            if (const auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([] {
                    if (auto* tes = RE::TES::GetSingleton()) {
                        tes->PurgeBufferedCells();
                        spdlog::info("stress: purged buffered cells (#{})", ++purges);
                    }
                });
            }
        }

        std::array<char, 4> TypeOf(RE::BGSStoryEventManager* manager, std::uint32_t index)
        {
            std::array<char, 4> type{ '?', '?', '?', '?' };
            if (manager && index < manager->registeredEvents.size()) {
                std::memcpy(type.data(), manager->registeredEvents[index].uniqueID, type.size());
            }
            return type;
        }

        // ID 32359: BGSStoryEventManager::AddEvent_Impl(manager, registered event index, event data).
        using AddEventFn = std::uint32_t (*)(RE::BGSStoryEventManager*, std::uint32_t, const void*);
        AddEventFn addEvent = nullptr;

        std::string Word(std::uintptr_t value)
        {
            const auto header = Forms::ReadHeader(value);
            if (!header || !Memory::InGameImage(header->vtable)) {
                return std::format("0x{:X}", value);
            }
            if (header->formType == std::to_underlying(RE::FormType::Location)) {
                return std::format("0x{:X} (BGSLocation {:08X} \"{}\")", value, header->formID,
                    reinterpret_cast<RE::BGSLocation*>(value)->GetName());
            }
            return std::format("0x{:X} ({} {:08X})", value, Memory::ClassName(header->vtable), header->formID);
        }

        std::uint32_t AddEvent(RE::BGSStoryEventManager* manager, std::uint32_t index, const void* event)
        {
            const auto type = TypeOf(manager, index);
            const bool changeLocationEvent = std::string_view{ type.data(), type.size() } == "CLOC";
            if (changeLocationEvent) {
                auto& record = changeLocation.Next();
                record.time = GetTickCount64();
                record.thread = GetCurrentThreadId();
                record.index = index;
                record.type = type;
                record.payload.fill(0);
                Memory::Read(reinterpret_cast<std::uintptr_t>(event), record.payload.data(), sizeof(record.payload));
                record.stack.Capture();

            }
            const auto result = addEvent(manager, index, event);
            if (changeLocationEvent && purge) {
                QueuePurge();
            }
            return result;
        }
    }

    void Install(bool stressPurge)
    {
        static constexpr std::uint8_t prologue[]{ 0x40, 0x57, 0x41, 0x56, 0x41, 0x57 };
        addEvent = Detour::Entry("BGSStoryEventManager::AddEvent (history)", REL::ID(32359), prologue, &AddEvent);
        installed = addEvent != nullptr;
        if (installed) {
            purge = stressPurge;
            spdlog::info("tracking who queues Change Location story events");
            if (purge) {
                spdlog::warn("STRESS MODE: purging buffered cells after every Change Location event; for "
                             "reproducing the crash, not for normal play");
            }
        }
    }

    std::string Recent()
    {
        if (!installed) {
            return {};
        }
        const auto now = GetTickCount64();
        std::string out;
        std::size_t shown = 0;
        changeLocation.Scan([&](const Record& record) {
            out += std::format("\n    CLOC queued {} ms ago on {}; event data:", now - record.time,
                Report::ThreadName(record.thread));
            for (const auto word : record.payload) {
                out += " " + Word(word);
            }
            out += "; queued from:" + Report::StackTrace(record.stack.View());
            return ++shown < Shown;
        });
        return out;
    }
}
