#include "Horo/Runtime/Scene/PersistentEntityIdentity.h"
#include "SceneTestIdentity.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using SceneTest::Id;

        PersistentEntityRecord Authored(const std::uint8_t identity, const std::uint64_t object,
                                        const PersistentEntityDisposition disposition = PersistentEntityDisposition::Live,
                                        const std::uint64_t generation = 1) {
            return {.identity = Id<PersistentEntityId>(identity),
                    .generation = {generation},
                    .provenance = AuthoredPersistentEntityProvenance{SceneDefinitionId{7}, SceneObjectId{object}},
                    .disposition = disposition};
        }

        PersistentEntityRecord Spawned(const std::uint8_t identity, const std::uint8_t definition,
                                       const std::optional<std::uint8_t> prefab = std::nullopt) {
            std::optional<SavePrefabInstanceId> prefabInstance;
            if (prefab.has_value())
                prefabInstance = Id<SavePrefabInstanceId>(*prefab);
            return {.identity = Id<PersistentEntityId>(identity),
                    .generation = {1},
                    .provenance = SpawnedPersistentEntityProvenance{Id<SaveAssetId>(definition), prefabInstance},
                    .disposition = PersistentEntityDisposition::Live};
        }

        PersistentEntityRuntimeBinding Binding(const std::uint8_t identity, const std::uint32_t slot, const std::uint64_t generation = 1,
                                               const std::uint64_t runtime = 9) {
            return {.identity = Id<PersistentEntityId>(identity),
                    .generation = {generation},
                    .runtime = EntityRef{SceneRuntimeId{runtime}, EntityId{slot, 1}}};
        }

        void RequireCode(const auto &result, const std::string_view code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code);
        }

        static_assert(!std::is_same_v<decltype(PersistentEntityRecord::identity), EntityRef>);

        TEST_CASE("Persistent Scene identities canonicalize independently from restore iteration order",
                  "[runtime][scene][save][identity]") {
            const std::array records{Spawned(3, 30, 31), Authored(1, 10), Authored(2, 20)};
            const std::array bindings{Binding(2, 4), Binding(3, 8), Binding(1, 2)};
            auto first = PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, records, bindings);
            REQUIRE(first.HasValue());

            auto reversedRecords = std::vector<PersistentEntityRecord>{records.rbegin(), records.rend()};
            auto reversedBindings = std::vector<PersistentEntityRuntimeBinding>{bindings.rbegin(), bindings.rend()};
            auto second = PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, reversedRecords, reversedBindings);
            REQUIRE(second.HasValue());

            REQUIRE(first.Value().Records().size() == 3);
            REQUIRE(second.Value().Records().size() == 3);
            for (std::size_t index = 0; index < first.Value().Records().size(); ++index)
                CHECK(first.Value().Records()[index].identity == second.Value().Records()[index].identity);
            CHECK(first.Value().Records()[0].identity == Id<PersistentEntityId>(1));
            CHECK(first.Value().Records()[1].identity == Id<PersistentEntityId>(2));
            CHECK(first.Value().Records()[2].identity == Id<PersistentEntityId>(3));
            REQUIRE(first.Value().FindAuthored(SceneDefinitionId{7}, SceneObjectId{20}) != nullptr);
            CHECK(first.Value().FindAuthored(SceneDefinitionId{7}, SceneObjectId{20})->identity == Id<PersistentEntityId>(2));
        }

        TEST_CASE("Live persistent identities remap bidirectionally without persisting ECS handles", "[runtime][scene][save][identity]") {
            const std::array records{Authored(1, 10), Spawned(2, 20)};
            const std::array bindings{Binding(1, 11), Binding(2, 12)};
            auto map = PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, records, bindings);
            REQUIRE(map.HasValue());
            REQUIRE(map.Value().Resolve(Id<PersistentEntityId>(1), PersistentEntityGeneration{1}).HasValue());
            CHECK(map.Value().Resolve(Id<PersistentEntityId>(1), PersistentEntityGeneration{1}).Value() == bindings[0].runtime);
            REQUIRE(map.Value().Find(bindings[1].runtime).HasValue());
            CHECK(map.Value().Find(bindings[1].runtime).Value() == Id<PersistentEntityId>(2));
            RequireCode(map.Value().Find(EntityRef{SceneRuntimeId{10}, EntityId{12, 1}}), "scene.persistence.binding_invalid");
        }

        TEST_CASE("Tombstones remain durable and cannot accidentally materialize destroyed entities", "[runtime][scene][save][identity]") {
            const std::array records{Authored(1, 10, PersistentEntityDisposition::Tombstone, 4)};
            auto map = PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, records, std::span<const PersistentEntityRuntimeBinding>{});
            REQUIRE(map.HasValue());
            REQUIRE(map.Value().FindAuthored(SceneDefinitionId{7}, SceneObjectId{10}) != nullptr);
            CHECK(map.Value().FindAuthored(SceneDefinitionId{7}, SceneObjectId{10})->disposition == PersistentEntityDisposition::Tombstone);
            RequireCode(map.Value().Resolve(Id<PersistentEntityId>(1), PersistentEntityGeneration{4}),
                        "scene.persistence.identity_tombstoned");

            const std::array illegalBinding{Binding(1, 2, 4)};
            RequireCode(PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, records, illegalBinding),
                        "scene.persistence.identity_tombstoned");
        }

        TEST_CASE("Duplicate durable and authored identities fail with actionable diagnostics", "[runtime][scene][save][identity]") {
            const std::array duplicateIdentity{Authored(1, 10), Authored(1, 20)};
            const std::array duplicateIdentityBindings{Binding(1, 2), Binding(1, 3)};
            auto duplicate = PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, duplicateIdentity, duplicateIdentityBindings);
            RequireCode(duplicate, "scene.persistence.identity_duplicate");
            CHECK(duplicate.ErrorValue().message.find(Id<PersistentEntityId>(1).ToString()) != std::string::npos);

            const std::array duplicateAuthored{Authored(1, 10), Authored(2, 10)};
            const std::array duplicateAuthoredBindings{Binding(1, 2), Binding(2, 3)};
            auto authored = PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, duplicateAuthored, duplicateAuthoredBindings);
            RequireCode(authored, "scene.persistence.identity_duplicate");
            CHECK(authored.ErrorValue().message.find("7:10") != std::string::npos);
        }

        TEST_CASE("Restore rejects stale missing duplicate and foreign runtime bindings", "[runtime][scene][save][identity]") {
            const std::array records{Authored(1, 10, PersistentEntityDisposition::Live, 2)};
            RequireCode(PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, records, std::span<const PersistentEntityRuntimeBinding>{}),
                        "scene.persistence.binding_missing");

            const std::array stale{Binding(1, 2, 1)};
            RequireCode(PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, records, stale), "scene.persistence.identity_stale");

            const std::array foreign{Binding(1, 2, 2, 10)};
            RequireCode(PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, records, foreign), "scene.persistence.binding_invalid");

            const std::array valid{Binding(1, 2, 2)};
            auto map = PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, records, valid);
            REQUIRE(map.HasValue());
            RequireCode(map.Value().Resolve(Id<PersistentEntityId>(1), PersistentEntityGeneration{1}), "scene.persistence.identity_stale");
            RequireCode(map.Value().Resolve(Id<PersistentEntityId>(9), PersistentEntityGeneration{1}),
                        "scene.persistence.identity_unknown");
        }

        TEST_CASE("Persistent identity candidates reject invalid bounds provenance and ambiguous ownership",
                  "[runtime][scene][save][identity]") {
            const std::array records{Authored(1, 10)};
            const std::array valid{Binding(1, 2)};
            RequireCode(PersistentEntityIdentityMap::Create(SceneRuntimeId{}, records, valid), "scene.persistence.identity_invalid");
            RequireCode(PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, records, valid, 0), "scene.persistence.identity_invalid");

            auto invalidGeneration = Authored(1, 10, PersistentEntityDisposition::Live, 0);
            RequireCode(PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, std::span{&invalidGeneration, 1}, valid),
                        "scene.persistence.identity_invalid");
            auto invalidSpawn = Spawned(1, 20);
            invalidSpawn.provenance = SpawnedPersistentEntityProvenance{};
            RequireCode(PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, std::span{&invalidSpawn, 1}, valid),
                        "scene.persistence.identity_invalid");

            const std::array unknownBinding{Binding(2, 2)};
            RequireCode(PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, records, unknownBinding),
                        "scene.persistence.binding_invalid");
            const std::array duplicateBindings{Binding(1, 2), Binding(1, 3)};
            RequireCode(PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, records, duplicateBindings),
                        "scene.persistence.identity_duplicate");
            const std::array twoRecords{Authored(1, 10), Authored(2, 20)};
            const std::array sharedRuntime{Binding(1, 2), Binding(2, 2)};
            RequireCode(PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, twoRecords, sharedRuntime),
                        "scene.persistence.identity_duplicate");
        }

        TEST_CASE("Persistent identity lookup rejects invalid and untracked references", "[runtime][scene][save][identity]") {
            const std::array records{Authored(1, 10)};
            const std::array bindings{Binding(1, 2)};
            auto map = PersistentEntityIdentityMap::Create(SceneRuntimeId{9}, records, bindings);
            REQUIRE(map.HasValue());

            RequireCode(map.Value().Resolve(PersistentEntityId{}, PersistentEntityGeneration{1}), "scene.persistence.identity_invalid");
            RequireCode(map.Value().Find(EntityRef{SceneRuntimeId{9}, EntityId{3, 1}}), "scene.persistence.identity_unknown");
            CHECK(map.Value().FindAuthored(SceneDefinitionId{}, SceneObjectId{10}) == nullptr);
            CHECK(map.Value().FindAuthored(SceneDefinitionId{7}, SceneObjectId{11}) == nullptr);
        }
    }  // namespace
}  // namespace Horo::Runtime
