#include "ProjectCollisionTemplate.h"

#include "CollisionIdentityGeneration.h"
#include "Horo/Editor/CollisionProjectSettingsSession.h"

#include <stdexcept>

namespace Horo::Editor {
    namespace {
        /** @brief Generates new persistent UUIDv4 bytes once during explicit authoring, with no reserved runtime identity. */
        template <typename Id> Id NewIdentity() {
            auto bytes = AllocateCollisionIdentityBytes(nullptr);
            if (bytes.HasError())
                throw std::runtime_error("Collision identity entropy unavailable.");
            return Id::FromBytes(bytes.Value());
        }
    }  // namespace

    /** @copydoc CreateProjectCollisionTemplate */
    Result<Application::ProjectCollisionSettings> CreateProjectCollisionTemplate(std::string projectId) {
        try {
            const auto layer = NewIdentity<Physics::CollisionLayerId>();
            const auto profile = NewIdentity<Physics::CollisionProfileId>();
            const auto channel = NewIdentity<Physics::PhysicsQueryChannelId>();
            Application::ProjectCollisionSettingsInput input;
            input.projectId = std::move(projectId);
            input.schema.defaultProfile = profile;
            input.schema.layers.push_back({layer});
            input.schema.pairs.push_back({layer, layer, Physics::SimulationPairResponse::Block});
            input.schema.queryChannels.push_back({channel});
            input.schema.profiles.push_back({profile, layer, true, true, true, {{channel, Physics::CollisionQueryResponse::Block}}});
            // These are ordinary template display values; no runtime code compares or reserves the names.
            input.layers.push_back({layer, "Default", "Initial project simulation layer"});
            input.profiles.push_back({profile, "Default", "Initial project collider profile"});
            input.channels.push_back({channel, "Default", "Initial project query intent"});
            return Application::ProjectCollisionSettings::Create(std::move(input));
        } catch (const std::exception &) {
            return Result<Application::ProjectCollisionSettings>::Failure(
                MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity),
                          "Unable to generate the new project's collision template identities."));
        }
    }
}  // namespace Horo::Editor
