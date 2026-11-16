#pragma once

#include "plugin/Forms.h"

namespace StaleParentCellFix::Report
{
    // notifications: a HUD message the first time each guard fires in a session.
    void Init(int detailedPerSite, bool notifications);

    // "main thread" or "thread 576".
    std::string ThreadName(std::uint32_t id);

    struct Object
    {
        std::string_view label;
        std::uintptr_t address;
        Forms::Kind kind;
    };

    // Logs a crash that a guard prevented: in full (objects, free history, call stack) for the
    // first few at each site, then as counted one-liners. Flushed straight away.
    void Incident(std::string_view site, std::string_view reason, std::initializer_list<Object> objects);

    std::string StackTrace(std::span<void* const> frames);
}
