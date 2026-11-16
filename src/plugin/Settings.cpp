#include "plugin/Settings.h"

namespace StaleParentCellFix::Settings
{
    Values Load()
    {
        constexpr auto path = "Data\\SKSE\\Plugins\\StaleParentCellFix.ini";
        const auto integer = [&](const char* section, const char* key, int fallback) {
            return static_cast<int>(GetPrivateProfileIntA(section, key, fallback, path));
        };
        Values values;
        values.unlink = integer("Fix", "bUnlinkPersistentReferences", values.unlink) != 0;
        values.guards = integer("Guards", "bEnabled", values.guards) != 0;
        values.trackFrees = integer("Diagnostics", "bTrackFrees", values.trackFrees) != 0;
        values.trackDeletes = integer("Diagnostics", "bTrackDeletes", values.trackDeletes) != 0;
        values.probe = integer("Diagnostics", "bProbeAccessViolations", values.probe) != 0;
        values.trackStoryEvents = integer("Diagnostics", "bTrackStoryEvents", values.trackStoryEvents) != 0;
        values.stressPurge = integer("Stress", "bPurgeCellsOnLocationChange", values.stressPurge) != 0;
        values.notifications = integer("Diagnostics", "bNotifications", values.notifications) != 0;
        values.detailedPerSite = integer("Diagnostics", "iDetailedReportsPerSite", values.detailedPerSite);
        values.probeReports = integer("Diagnostics", "iProbeReports", values.probeReports);
        spdlog::info("settings: unlink {} guards {} trackFrees {} trackDeletes {} trackStoryEvents {} stressPurge {} probe {} ({} reports) notifications {} detailedPerSite {}",
            values.unlink, values.guards, values.trackFrees, values.trackDeletes, values.trackStoryEvents, values.stressPurge, values.probe, values.probeReports, values.notifications,
            values.detailedPerSite);
        return values;
    }
}
