#include "plugin/Deletes.h"
#include "plugin/Events.h"
#include "plugin/Forms.h"
#include "plugin/Frees.h"
#include "plugin/Guards.h"
#include "plugin/Probe.h"
#include "plugin/Report.h"
#include "plugin/Settings.h"
#include "plugin/Unlink.h"

SKSEPluginInfo(
    .Version = REL::Version{ SPCF_VERSION_MAJOR, SPCF_VERSION_MINOR, SPCF_VERSION_PATCH, 0 },
    .Name = "StaleParentCellFix",
    .Author = "Nick",
    .StructCompatibility = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse, { .logPattern = "[%T.%e] [%L] %v", .trampoline = true, .trampolineSize = 1024 });
    spdlog::info("StaleParentCellFix {} loading on Skyrim {}", SPCF_VERSION_STRING,
        REL::Module::get().version().string("."));

    if (!REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_6_629)) {
        spdlog::critical("only Skyrim 1.6.629+ (AE) layouts are known; not installing");
        return false;
    }
    const auto settings = StaleParentCellFix::Settings::Load();
    StaleParentCellFix::Report::Init(settings.detailedPerSite, settings.notifications);
    StaleParentCellFix::Forms::Init();
    if (settings.trackFrees) {
        StaleParentCellFix::Frees::Install();
    }
    if (settings.trackDeletes) {
        StaleParentCellFix::Deletes::Install();
    }
    if (settings.unlink) {
        StaleParentCellFix::Unlink::Install();
    }
    if (settings.trackStoryEvents) {
        StaleParentCellFix::Events::Install(settings.stressPurge);
    }
    if (settings.guards) {
        StaleParentCellFix::Guards::Install();
    }
    if (settings.probe) {
        StaleParentCellFix::Probe::Install(settings.probeReports);
    }
    return true;
}
