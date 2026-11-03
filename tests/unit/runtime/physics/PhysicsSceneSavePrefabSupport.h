#pragma once
#include "EditorPlaySavePrefabServices.h"
#include "Horo/Application/ProjectVersion.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "Horo/Scene/SceneSource.h"
#include "PhysicsSceneStateTestSupport.h"
#include "PrefabTemplateProviderTestSupport.h"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <vector>

namespace Horo::Physics::StateTest {
    namespace {
        class PhysicsSavePrefabProjector final : public Prefab::PrefabComponentProjector {
        public:
            Result<bool> Project(const Prefab::RawComponentPayload &source, Runtime::RuntimeComponentSet &components,
                                 std::optional<Runtime::PrimitiveMeshDescriptor> &primitive) const override {
                return SceneSource::ProjectPrefabBuiltinComponent(source, components, primitive);
            }
        };

        Prefab::RawComponentPayload PhysicsPrefabField(Runtime::BuiltinSceneComponentKind kind, std::uint64_t occurrence,
                                                       const nlohmann::json &payload) {
            const auto rows = Runtime::BuiltinSceneComponentSchemas();
            const auto schema = std::ranges::find(rows, kind, &Runtime::BuiltinSceneComponentSchema::kind);
            REQUIRE(schema != rows.end());
            Prefab::RawComponentPayload result{Take(Prefab::PrefabComponentInstanceId::Create(occurrence)),
                                               {.typeId = Take(Gameplay::ComponentTypeId::Parse(schema->typeId)),
                                                .schemaVersion = SceneSource::PrefabBuiltinComponentPayloadSchemaVersion}};
            const auto encoded = payload.dump();
            result.component.payload.assign(reinterpret_cast<const std::byte *>(encoded.data()),
                                            reinterpret_cast<const std::byte *>(encoded.data() + encoded.size()));
            return result;
        }

        void RequirePhysicsPrefabProjection(const Prefab::RawComponentPayload &member) {
            Runtime::RuntimeComponentSet destination;
            std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
            auto result = SceneSource::ProjectPrefabBuiltinComponent(member, destination, primitive);
            std::string errors;
            for (const Error *error = result.HasError() ? &result.ErrorValue() : nullptr; error; error = error->cause.Get())
                errors += std::string{error->code.Value()} + ": " + error->message + "\n";
            INFO(errors);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value());
        }

        std::vector<Prefab::RawComponentPayload> PhysicsPrefabMembers(Assets::AssetId material, Math::Vec3 colliderTranslation) {
            SceneSource::SceneObjectComponentSet components;
            components.rigidBody = Runtime::RigidBodyComponent{.id = {61},
                                                               .body = {701},
                                                               .motion = Runtime::AuthoredPhysicsMotionType::Dynamic,
                                                               .mass = Runtime::AuthoredPhysicsMass{1.0F},
                                                               .initialLinearVelocity = {0.25F, 0.0F, 0.0F}};
            components.colliders.push_back({.id = {62},
                                            .collider = {702},
                                            .body = {.object = {0}, .body = {701}},
                                            .localPose = {.translation = colliderTranslation},
                                            .collisionProfile = Profile(),
                                            .materials = {{PhysicsMaterialSlotId::FromValue(1), material}}});
            const std::array source{SceneSource::SceneObjectSnapshot{{1}, {}, "Physics root", {}, {}, components}};
            const auto encoded = SceneSource::EncodeSceneSource({source, {}});
            const auto json = nlohmann::json::parse(encoded);
            const auto &fields = json.at("objects").at(0).at("components");
            std::vector members{PhysicsPrefabField(Runtime::BuiltinSceneComponentKind::RigidBody, 61, fields.at("rigidBody")),
                                PhysicsPrefabField(Runtime::BuiltinSceneComponentKind::Collider, 62, fields.at("colliders").at(0))};
            for (const auto &member : members)
                RequirePhysicsPrefabProjection(member);
            return members;
        }

        std::vector<std::uint8_t> CookPhysicsSavePrefab(const Assets::AssetRegistrySnapshot &registry,
                                                        const Prefab::PrefabLimitProfile &profile, Assets::AssetId material,
                                                        std::span<const std::uint8_t> materialArtifact, Math::Vec3 colliderTranslation) {
            Prefab::PrefabObjectNode root{.localId = {0},
                                          .name = "real-physics-root",
                                          .components = PhysicsPrefabMembers(material, colliderTranslation)};
            Prefab::PrefabObjectNode child{.localId = {7}, .parentLocalId = Prefab::LocalObjectId{0}, .name = "real-child"};
            auto document = Take(Prefab::PrefabDocument::Create({.projectVersion = Take(Application::ParseHoroVersion("1.2.3")),
                                                                 .assetId = Prefab::Test::Asset(),
                                                                 .objects = {std::move(root), std::move(child)},
                                                                 .referencedAssets = {material}},
                                                                profile));
            const auto source = Take(document.SerializeCanonical());
            const Prefab::PrefabSourceRevision revision{document.Data().projectVersion,
                                                        ComputeSha256(std::as_bytes(std::span{source.data(), source.size()}))};
            auto resolver = Take(Prefab::BuildPrefabSourceResolverSnapshot(registry, {{document, revision}}, profile));
            const Prefab::PrefabTemplateCookResource dependency{material, materialArtifact};
            auto cooked = Take(Prefab::CookPrefabTemplate(resolver, registry, Prefab::Test::Asset(), std::span{&dependency, 1},
                                                          Prefab::TestSupport::Target(), profile));
            REQUIRE(cooked.Data().dependencies.size() == 1);
            CHECK(cooked.Data().dependencies.front().asset.id == material);
            CHECK(cooked.Data().dependencies.front().artifactDigest == ComputeSha256(std::as_bytes(materialArtifact)));
            static_cast<void>(Take(Prefab::CookedPrefab::Parse(cooked.Bytes(), Prefab::Test::Asset(), profile)));
            std::vector<std::uint8_t> bytes;
            for (auto value : cooked.Bytes())
                bytes.push_back(std::to_integer<std::uint8_t>(value));
            return Prefab::TestSupport::Envelope(Prefab::Test::Asset(), "core.prefab", std::move(bytes));
        }
    }  // namespace
}  // namespace Horo::Physics::StateTest
