#include "ProjectCollisionTemplate.h"

#include "CollisionIdentityGeneration.h"
#include "Horo/Editor/CollisionProjectSettingsSession.h"

#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Generates new persistent UUIDv4 bytes once during explicit authoring, with no reserved runtime identity. */
        template <typename Id> Result<Id> NewIdentity() {
            auto bytes = AllocateCollisionIdentityBytes(nullptr);
            if (bytes.HasError())
                return Result<Id>::Failure(bytes.ErrorValue());
            return Result<Id>::Success(Id::FromBytes(bytes.Value()));
        }
    }  // namespace

    /** @copydoc CreateProjectCollisionTemplate */
    Result<Application::ProjectCollisionSettings> CreateProjectCollisionTemplate(std::string projectId) {
        try {
            const auto layerIdentity = NewIdentity<Physics::CollisionLayerId>();
            if (layerIdentity.HasError())
                return Result<Application::ProjectCollisionSettings>::Failure(layerIdentity.ErrorValue());
            const auto layer = layerIdentity.Value();
            const auto profileIdentity = NewIdentity<Physics::CollisionProfileId>();
            if (profileIdentity.HasError())
                return Result<Application::ProjectCollisionSettings>::Failure(profileIdentity.ErrorValue());
            const auto profile = profileIdentity.Value();
            const auto channelIdentity = NewIdentity<Physics::PhysicsQueryChannelId>();
            if (channelIdentity.HasError())
                return Result<Application::ProjectCollisionSettings>::Failure(channelIdentity.ErrorValue());
            const auto channel = channelIdentity.Value();
            Application::ProjectCollisionSettingsInput input;
            input.projectId = std::move(projectId);
            input.schema.defaultProfile = profile;
            input.schema.layers.push_back({layer});
            input.schema.pairs.emplace_back(layer, layer, Physics::SimulationPairResponse::Block);
            input.schema.queryChannels.push_back({channel});
            input.schema.profiles.push_back({profile, layer, true, true, true, {{channel, Physics::CollisionQueryResponse::Block}}});
            // These are ordinary template display values; no runtime code compares or reserves the names.
            input.layers.emplace_back(layer, "Default", "Initial project simulation layer");
            input.profiles.emplace_back(profile, "Default", "Initial project collider profile");
            input.channels.emplace_back(channel, "Default", "Initial project query intent");
            return Application::ProjectCollisionSettings::Create(std::move(input));
        } catch (const std::bad_alloc &) {
            return Result<Application::ProjectCollisionSettings>::Failure(
                MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity),
                          "Unable to generate the new project's collision template identities."));
        }
    }
}  // namespace Horo::Editor
