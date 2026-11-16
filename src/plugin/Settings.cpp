#include "plugin/Settings.h"

namespace StaleParentCellFix::Settings
{
    Values Load()
    {
        constexpr auto path = "Data\\SKSE\\Plugins\\StaleParentCellFix.ini";
        Values values;
        values.notifications = GetPrivateProfileIntA("General", "bNotifications", values.notifications, path) != 0;
        spdlog::info("settings: notifications {}", values.notifications);
        return values;
    }
}
