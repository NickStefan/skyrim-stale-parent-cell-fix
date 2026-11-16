#pragma once

namespace StaleParentCellFix::Report
{
    void Init(bool notifications);

    // A HUD message the first time each kind of event happens in a session, if notifications are on.
    void Notice(std::string_view kind, std::string text);
}
