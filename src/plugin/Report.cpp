#include "plugin/Report.h"

#include <set>

namespace StaleParentCellFix::Report
{
    namespace
    {
        bool notify = true;
        std::mutex lock;
        std::set<std::string, std::less<>> noticed;
    }

    void Init(bool notifications)
    {
        notify = notifications;
    }

    void Notice(std::string_view kind, std::string text)
    {
        {
            std::scoped_lock guard{ lock };
            if (!notify || !noticed.emplace(kind).second) {
                return;
            }
        }
        // The HUD is main-thread only.
        if (const auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([text = std::move(text)] { RE::SendHUDMessage::ShowHUDMessage(text.c_str()); });
        }
        spdlog::default_logger()->flush();
    }
}
