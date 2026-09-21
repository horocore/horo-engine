#include "editor/document/NavigationAgentJson.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace Horo::Editor::Detail {
    namespace {
        using Json = nlohmann::json;

        [[nodiscard]] Result<Runtime::NavigationAgentComponent> InvalidAgentJson() {
            return Result<Runtime::NavigationAgentComponent>::Failure(MakeError(Navigation::NavigationErrors::AgentDescriptorInvalid));
        }
    }  // namespace

    /** @copydoc ParseNavigationAgentJson */
    Result<Runtime::NavigationAgentComponent> ParseNavigationAgentJson(const Json &value) {
        try {
            if (!value.is_object() || !value.contains("schemaVersion") || !value["schemaVersion"].is_number_unsigned() ||
                !value.contains("profile") || !value["profile"].is_number_unsigned() || !value.contains("filter") ||
                !value["filter"].is_number_unsigned() || !value.contains("radiusOverride") ||
                (!value["radiusOverride"].is_null() && !value["radiusOverride"].is_number()) || !value.contains("movementCapability") ||
                !value["movementCapability"].is_string() || (value.contains("enabled") && !value["enabled"].is_boolean()))
                return InvalidAgentJson();

            const auto profile = Navigation::NavigationAgentProfileId::Create(value["profile"].get<std::uint64_t>());
            const auto filter = Navigation::NavigationFilterId::Create(value["filter"].get<std::uint64_t>());
            if (profile.HasError() || filter.HasError() || value["schemaVersion"].get<std::uint32_t>() != 1 ||
                value["movementCapability"].get<std::string>() != "grounded")
                return InvalidAgentJson();

            std::optional<float> radiusOverride;
            if (!value["radiusOverride"].is_null())
                radiusOverride = value["radiusOverride"].get<float>();
            Runtime::NavigationAgentComponent agent{
                .schemaVersion = 1,
                .profile = profile.Value(),
                .filter = filter.Value(),
                .radiusOverride = radiusOverride,
                .movementCapability = Navigation::NavigationAgentMovementCapability::Grounded,
                .enabled = value.value("enabled", true),
            };
            if (Runtime::ValidateNavigationAgentComponent(agent).HasError())
                return InvalidAgentJson();
            return Result<Runtime::NavigationAgentComponent>::Success(std::move(agent));
        } catch (const nlohmann::json::exception &) {
            return InvalidAgentJson();
        }
    }
}  // namespace Horo::Editor::Detail
