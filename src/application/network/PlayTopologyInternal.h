#pragma once
#include "Horo/Application/PlayTopology.h"

namespace Horo::Application::Detail {
    [[nodiscard]] bool ValidPlayTopologyCatalog(const PlayTopologyCatalog &catalog);
    [[nodiscard]] bool ValidPlayTopologyOverrides(const PlayTopologyUserSettings &settings);
}  // namespace Horo::Application::Detail
