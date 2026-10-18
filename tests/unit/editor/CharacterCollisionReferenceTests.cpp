#include "CollisionSettingsTestSupport.h"

namespace Horo::Tests::CollisionSettings {
    namespace {
        SceneSource::SceneObjectSnapshot CharacterOwner() {
            Runtime::CharacterControllerComponent character{.id = {77}};
            return {.id = {7}, .name = "Disabled configured Character", .components = {.characterController = character}};
        }

        void CheckBlocked(Project &project, const SceneSource::SceneObjectSnapshot &owner,
                          Application::ProjectCollisionSettingsInput candidate, const Editor::CollisionImpactReason reason) {
            const std::array objects{owner};
            const auto source = SceneSource::EncodeSceneSource({objects, {}});
            const auto decoded = SceneSource::DecodeSceneSource(source);
            REQUIRE(decoded.HasValue());
            CHECK(decoded.Value().unsupportedComponentOwners.empty());
            project.Scene(source);
            auto session = project.Open();
            const auto before = session->Snapshot().Value();
            const auto review = session->Preview(std::move(candidate));
            REQUIRE(review.HasValue());
            CHECK_FALSE(review.Value().CanCommit());
            CHECK(std::ranges::any_of(review.Value().Impacts(), [reason](const auto &impact) {
                return impact.object == SceneSource::SceneObjectId{7} && impact.reason == reason && !impact.unsupported &&
                       impact.blocksPublication;
            }));
            CHECK(session->Commit(review.Value()).HasError());
            CHECK(session->Snapshot().Value() == before);
            CHECK_FALSE(session->IsDirty());
        }
    }  // namespace

    TEST_CASE("Disabled Character primary and required profile references prevent collision profile retirement",
              "[character][collision][scene][references]") {
        Project project;
        auto owner = CharacterOwner();
        SECTION("primary profile") {
            owner.components.characterController->collision.profile = IdOf<Physics::CollisionProfileId>(3);
        }
        SECTION("required profile") {
            owner.components.characterController->collision.requiredProfile = IdOf<Physics::CollisionProfileId>(3);
        }
        CheckBlocked(project, owner, RemoveSpare(Input()), Editor::CollisionImpactReason::ProfileReference);
    }

    TEST_CASE("Disabled Character required layer remains a typed collision retirement reference",
              "[character][collision][scene][references]") {
        auto input = Input();
        const auto layer = IdOf<Physics::CollisionLayerId>(10);
        input.schema.layers.push_back({.id = layer, .enabled = false});
        input.layers.push_back({layer, "Optional authored layer", {}});
        Project project{input};
        auto owner = CharacterOwner();
        owner.components.characterController->collision.requiredLayer = layer;
        std::erase_if(input.schema.layers, [layer](const auto &value) {
            return value.id == layer;
        });
        std::erase_if(input.layers, [layer](const auto &value) {
            return value.id == layer;
        });
        CheckBlocked(project, owner, std::move(input), Editor::CollisionImpactReason::LayerReference);
    }

    TEST_CASE("Disabled Character query intent remains a typed collision retirement reference",
              "[character][collision][scene][references]") {
        auto input = Input();
        const auto channel = IdOf<Physics::PhysicsQueryChannelId>(11);
        input.schema.queryChannels.push_back({.id = channel});
        input.channels.push_back({channel, "Optional authored query", {}});
        for (auto &profile : input.schema.profiles)
            profile.queryResponses.push_back({channel, Physics::CollisionQueryResponse::Ignore});
        Project project{input};
        auto owner = CharacterOwner();
        owner.components.characterController->collision.queryChannel = channel;
        std::erase_if(input.schema.queryChannels, [channel](const auto &value) {
            return value.id == channel;
        });
        std::erase_if(input.channels, [channel](const auto &value) {
            return value.id == channel;
        });
        for (auto &profile : input.schema.profiles)
            std::erase_if(profile.queryResponses, [channel](const auto &query) {
                return query.channel == channel;
            });
        CheckBlocked(project, owner, std::move(input), Editor::CollisionImpactReason::QueryChannelReference);
    }
}  // namespace Horo::Tests::CollisionSettings
