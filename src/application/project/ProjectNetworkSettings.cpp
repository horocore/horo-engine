#include "Horo/Application/ProjectNetworkSettings.h"

namespace Horo::Application {
    /** @copydoc ResolveProjectNetworkSettings */
    Result<std::optional<Network::NetworkProjectSettings>> ResolveProjectNetworkSettings(const ProjectSourceDocument &source) {
        using Settings = std::optional<Network::NetworkProjectSettings>;
        if (!source.networkSettingsSource)
            return Result<Settings>::Success(std::nullopt);
        auto parsed = Network::ParseNetworkProjectSettings(*source.networkSettingsSource);
        if (parsed.HasError())
            return Result<Settings>::Failure(parsed.ErrorValue());
        auto validated = Network::NetworkProjectSettings::Create(parsed.Value());
        if (validated.HasError())
            return Result<Settings>::Failure(validated.ErrorValue());
        return Result<Settings>::Success(Settings{std::move(validated).Value()});
    }
}  // namespace Horo::Application
