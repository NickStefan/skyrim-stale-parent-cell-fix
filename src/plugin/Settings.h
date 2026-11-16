#pragma once

namespace StaleParentCellFix::Settings
{
    struct Values
    {
        bool notifications = true;
    };

    // Data/SKSE/Plugins/StaleParentCellFix.ini; missing keys keep their defaults.
    Values Load();
}
