#pragma once

namespace StaleParentCellFix::Settings
{
    struct Values
    {
        bool guards = true;
        bool trackFrees = true;
        bool trackDeletes = true;
        bool probe = true;
        bool notifications = true;
        bool trackStoryEvents = true;
        bool stressPurge = false;
        bool unlink = true;
        int detailedPerSite = 20;
        int probeReports = 8;
    };

    // Data/SKSE/Plugins/StaleParentCellFix.ini; missing keys keep their defaults.
    Values Load();
}
