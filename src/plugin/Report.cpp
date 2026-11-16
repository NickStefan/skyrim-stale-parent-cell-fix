#include "plugin/Report.h"

#include <set>

#include "plugin/Events.h"
#include "plugin/Memory.h"

namespace StaleParentCellFix::Report
{
    namespace
    {
        std::uint32_t mainThread = 0;
        int detailed = 20;
        std::mutex lock;
        std::map<std::string, std::uint64_t, std::less<>> counts;
        std::uint64_t total = 0;
        bool notify = true;

        // The HUD is main-thread only; incidents can come from the story manager or cell loader.
        void Notify(std::string text)
        {
            if (const auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([text = std::move(text)] { RE::SendHUDMessage::ShowHUDMessage(text.c_str()); });
            }
        }
    }

    void Init(int detailedPerSite, bool notifications)
    {
        mainThread = GetCurrentThreadId();
        detailed = detailedPerSite;
        notify = notifications;
    }

    std::string ThreadName(std::uint32_t id)
    {
        return id == mainThread ? "main thread"s : std::format("thread {}", id);
    }

    std::string StackTrace(std::span<void* const> frames)
    {
        std::string out;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            out += std::format("\n      [{:2}] {}", i, Memory::Symbol(reinterpret_cast<std::uintptr_t>(frames[i])));
        }
        return out;
    }

    void Incident(std::string_view site, std::string_view reason, std::initializer_list<Object> objects)
    {
        void* frames[48]{};
        const auto depth = RtlCaptureStackBackTrace(1, static_cast<DWORD>(std::size(frames)), frames, nullptr);
        const auto thread = GetCurrentThreadId();

        std::scoped_lock guard{ lock };
        auto it = counts.find(site);
        if (it == counts.end()) {
            it = counts.emplace(std::string{ site }, 0).first;
        }
        const auto count = ++it->second;
        ++total;
        if (notify && count == 1) {
            Notify(std::format("StaleParentCellFix: a guard prevented a crash the fix missed (#{}, {}). Log "
                               "written; consider reloading instead of saving.",
                total, site.substr(0, site.find(' '))));
        }
        if (count > static_cast<std::uint64_t>(detailed)) {
            if (count % 100 == 0) {
                spdlog::warn("PREVENTED #{} at {} (seen {} times here; last: {} on {})", total, site, count, reason,
                    ThreadName(thread));
            }
            return;
        }
        std::string text = std::format("PREVENTED #{} at {} (#{} here) on {}\n    reason: {}", total, site, count,
            ThreadName(thread), reason);
        for (const auto& object : objects) {
            text += Forms::Describe(object.label, object.address, object.kind);
        }
        text += "\n    call stack:" + StackTrace({ frames, depth });
        if (const auto events = Events::Recent(); !events.empty()) {
            text += "\n    recent Change Location events:" + events;
        }
        spdlog::warn("{}", text);
        if (count == static_cast<std::uint64_t>(detailed)) {
            spdlog::warn("{}: further incidents here are only counted (every 100th is logged)", site);
        }
        spdlog::default_logger()->flush();
    }

    void Notice(std::string_view kind, std::string text)
    {
        static std::mutex noticeLock;
        static std::set<std::string, std::less<>> noticed;
        {
            std::scoped_lock guard{ noticeLock };
            if (!notify || !noticed.emplace(kind).second) {
                return;
            }
        }
        Notify(std::move(text));
        spdlog::default_logger()->flush();
    }
}
