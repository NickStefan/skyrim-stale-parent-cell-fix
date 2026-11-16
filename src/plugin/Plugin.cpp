#include "plugin/Release.h"
#include "plugin/Report.h"
#include "plugin/Settings.h"

SKSEPluginInfo(
    .Version = REL::Version{ SPCF_VERSION_MAJOR, SPCF_VERSION_MINOR, SPCF_VERSION_PATCH, 0 },
    .Name = "StaleParentCellFix",
    .Author = "Nick",
    .StructCompatibility = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse, { .logPattern = "[%T.%e] [%L] %v", .trampoline = true, .trampolineSize = 64 });
    spdlog::info("StaleParentCellFix {} loading on Skyrim {}", SPCF_VERSION_STRING,
        REL::Module::get().version().string("."));

    if (!REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_6_629)) {
        spdlog::critical("only Skyrim 1.6.629+ (AE) is verified; not installing");
        return false;
    }
    const auto settings = StaleParentCellFix::Settings::Load();
    StaleParentCellFix::Report::Init(settings.notifications);
    StaleParentCellFix::Release::Install();
    return true;
}
