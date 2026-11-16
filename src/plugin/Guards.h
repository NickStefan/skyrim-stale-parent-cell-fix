#pragma once

// Validation hooks on the engine functions the stale parentCell crashes fault in. Each one
// checks that the reference, cell or worldspace it is handed is still a live object; when it is
// not, the call is answered as if the object had no location/zone (or the stale link is cut)
// instead of following freed memory, and the incident is logged.
namespace StaleParentCellFix::Guards
{
    void Install();
}
