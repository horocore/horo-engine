#pragma once
#include "Horo/Application/NavigationProjectSettings.h"
#include "NavigationSettingsTestSupport.h"

namespace Horo::Tests::NavigationSettingsSupport {
    inline Application::NavigationProjectSettingsInput Policy() {
        return {1,
                Asset(1),
                "recast_detour",
                {Id<N::NavigationProjectProfileId>(1),
                 Id<N::NavigationProjectProfileRevision>(1),
                 {64, 8, 128, 8, 1024 * 1024, 128 * 1024 * 1024, 8192},
                 {N::NavigationQueryKind::Path, N::NavigationQualityLevel::Balanced, {1024, 128, 1000}}}};
    }

}  // namespace Horo::Tests::NavigationSettingsSupport
