#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Prefab {
    namespace {
        using Test::Asset;

        Application::HoroVersion ProjectVersion() {
            return Application::ParseHoroVersion("1.2.3").Value();
        }

        PrefabSourceRevision Revision() {
            Sha256Digest digest{};
            digest.bytes.back() = 7;
            return {.projectVersion = ProjectVersion(), .contentDigest = digest};
        }

        Gameplay::ComponentTypeId ComponentType() {
            return Gameplay::ComponentTypeId::Parse("game.tests.prefab_document").Value();
        }

        Gameplay::BehaviorTypeId BehaviorType() {
            return Gameplay::BehaviorTypeId::Parse("game.tests.prefab_behavior").Value();
        }

        Gameplay::GameAssetTypeId GameAssetType() {
            return Gameplay::GameAssetTypeId::Parse("game.tests.prefab_asset").Value();
        }

        Assets::AssetRegistrySnapshot AssetProviders() {
            Assets::AssetRegistry registry;
            std::vector<Assets::AssetRecord> records;
            records.push_back({Asset(2), Assets::AssetTypeId::Parse(GameAssetType().Value()).Value(),
                               ProjectPath::Parse("assets/prefab/test.asset").Value(),
                               ProjectPath::Parse("assets/prefab/test.asset.meta").Value()});
            REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
            return registry.Snapshot();
        }

        RawComponentPayload Component(const std::uint64_t instance, const std::size_t payloadBytes = 0) {
            return {
                .instance = PrefabComponentInstanceId::Create(instance).Value(),
                .component = {.typeId = ComponentType(), .schemaVersion = 1, .payload = std::vector<std::byte>(payloadBytes)},
            };
        }

        Gameplay::BehaviorComponent Behavior(const std::uint64_t instance) {
            return {
                .instanceId = {instance},
                .typeId = BehaviorType(),
                .schemaVersion = 1,
                .enabled = true,
                .fields = {{.name = "speed", .value = 2.0}},
            };
        }

        Gameplay::ComponentDescriptor ComponentDescriptor(const std::uint32_t schemaVersion = 1) {
            return {.typeId = ComponentType(), .schemaVersion = schemaVersion, .displayName = "Prefab Component", .category = "Tests"};
        }

        Gameplay::BehaviorDescriptor BehaviorDescriptor(const std::uint32_t schemaVersion = 1) {
            return {.typeId = BehaviorType(),
                    .schemaVersion = schemaVersion,
                    .displayName = "Prefab Behavior",
                    .category = "Tests",
                    .fields = {{.name = "speed", .defaultValue = 1.0}}};
        }

        Gameplay::SerializedGameAsset GameAsset(const std::uint32_t schemaVersion = 1) {
            return {.typeId = GameAssetType(),
                    .schemaVersion = schemaVersion,
                    .encoding = Gameplay::GameAssetPayloadEncoding::Binary,
                    .payload = {std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}}};
        }

        Result<Gameplay::SerializedGameAsset> ImportGameAsset(void *, const Gameplay::GameAssetImportInput &, const CancellationToken &) {
            return Result<Gameplay::SerializedGameAsset>::Success(GameAsset());
        }

        Result<Gameplay::SerializedGameAsset> SerializeGameAsset(void *, const Gameplay::GameAssetSerializationInput &,
                                                                 const CancellationToken &) {
            return Result<Gameplay::SerializedGameAsset>::Success(GameAsset());
        }

        Result<std::vector<std::byte>> CookGameAsset(void *, const Gameplay::GameAssetCookInput &, const CancellationToken &) {
            return Result<std::vector<std::byte>>::Success({});
        }

        Gameplay::GameAssetTypeRegistration GameAssetRegistration(const std::uint32_t schemaVersion = 1) {
            return {
                .descriptor = {.typeId = GameAssetType(),
                               .schemaVersion = schemaVersion,
                               .sourceExtensions = {"prefabasset"},
                               .cookTargets = {AssetCookTargetId::Parse("headless-null").Value()},
                               .editor = {.displayName = "Prefab Asset", .category = "Tests", .iconName = "asset-test"}},
                .handler = {.importAsset = &ImportGameAsset, .serializeAsset = &SerializeGameAsset, .cookAsset = &CookGameAsset},
            };
        }

        Gameplay::ComponentRegistry ComponentProviders(const std::uint32_t schemaVersion = 0) {
            Gameplay::ComponentRegistry registry;
            if (schemaVersion != 0)
                REQUIRE(registry.Register(ComponentDescriptor(schemaVersion)).HasValue());
            REQUIRE(registry.Freeze().HasValue());
            return registry;
        }

        Gameplay::GameAssetTypeRegistry GameAssetProviders(const std::uint32_t schemaVersion = 0) {
            Gameplay::GameAssetTypeRegistry registry{"game.tests"};
            if (schemaVersion != 0)
                REQUIRE(registry.Register(GameAssetRegistration(schemaVersion)).HasValue());
            REQUIRE(registry.Freeze().HasValue());
            return registry;
        }

        PrefabObjectNode Root() {
            return {.localId = {}, .parentLocalId = std::nullopt, .name = "Root"};
        }

        PrefabDocumentData Concrete() {
            return {.projectVersion = ProjectVersion(), .assetId = Asset(1), .objects = {Root()}};
        }

        /** @brief Builds one concrete prefab carrying the shared project-provider payload fixture. */
        PrefabDocumentData ProviderDocumentData(std::vector<Gameplay::BehaviorComponent> behaviors, const std::size_t componentBytes = 0) {
            PrefabDocumentData data = Concrete();
            data.referencedAssets = {Asset(2)};
            data.objects.front().components = {Component(1, componentBytes)};
            data.objects.front().behaviors = std::move(behaviors);
            return data;
        }

        PrefabLimitProfile Limits() {
            return PrefabLimitProfile::Create({}).Value();
        }

        Result<PrefabDocument> CreateDocument(PrefabDocumentData data) {
            return PrefabDocument::Create(std::move(data), Limits());
        }

        TEST_CASE("Prefab document publishes one immutable ordered portable candidate", "[unit][prefab][document]") {
            auto data = ProviderDocumentData({Behavior(2)});
            data.objects.push_back({.localId = {8}, .parentLocalId = LocalObjectId{}, .name = "Child"});
            data.composition = PrefabComposition{
                .nestedPlacements = {{.placementLocalId = {12},
                                      .parentLocalId = LocalObjectId{8},
                                      .sourcePrefab = PrefabAssetReference::Create(Asset(2)).Value(),
                                      .authoredAgainst = Revision()}},
            };

            const auto document = CreateDocument(data);
            REQUIRE(document.HasValue());
            REQUIRE(document.Value().Data() == data);
            REQUIRE(document.Value().Data().objects.front().components.front().component.payload.empty());
        }

        TEST_CASE("Prefab hierarchy requires one root and parent-before-child stable slots", "[unit][prefab][document]") {
            auto rootMissing = Concrete();
            rootMissing.objects.front().localId = {1};
            REQUIRE(CreateDocument(rootMissing).HasError());

            auto duplicate = Concrete();
            duplicate.objects.push_back({.localId = {1}, .parentLocalId = LocalObjectId{}});
            duplicate.objects.push_back({.localId = {1}, .parentLocalId = LocalObjectId{}});
            REQUIRE(CreateDocument(duplicate).HasError());

            auto unordered = Concrete();
            unordered.objects.push_back({.localId = {2}, .parentLocalId = LocalObjectId{3}});
            unordered.objects.push_back({.localId = {3}, .parentLocalId = LocalObjectId{}});
            REQUIRE(CreateDocument(unordered).HasError());

            auto secondRoot = Concrete();
            secondRoot.objects.push_back(Root());
            REQUIRE(CreateDocument(secondRoot).HasError());
        }

        TEST_CASE("Prefab hierarchy accepts exact object and depth bounds then rejects overflow", "[unit][prefab][document]") {
            auto objectBound = Concrete();
            for (std::uint32_t id = 1; id < PrefabHardLimits::SourceObjectCount; ++id)
                objectBound.objects.push_back({.localId = {id}, .parentLocalId = LocalObjectId{}});
            REQUIRE(CreateDocument(objectBound).HasValue());
            objectBound.objects.push_back(
                {.localId = {static_cast<std::uint32_t>(PrefabHardLimits::SourceObjectCount)}, .parentLocalId = LocalObjectId{}});
            REQUIRE(CreateDocument(objectBound).HasError());

            auto depthBound = Concrete();
            for (std::uint32_t id = 1; id < PrefabHardLimits::SourceHierarchyDepth; ++id)
                depthBound.objects.push_back({.localId = {id}, .parentLocalId = LocalObjectId{id - 1}});
            REQUIRE(CreateDocument(depthBound).HasValue());
            depthBound.objects.push_back(
                {.localId = {static_cast<std::uint32_t>(PrefabHardLimits::SourceHierarchyDepth)},
                 .parentLocalId = LocalObjectId{static_cast<std::uint32_t>(PrefabHardLimits::SourceHierarchyDepth - 1)}});
            REQUIRE(CreateDocument(depthBound).HasError());
        }

        TEST_CASE("Prefab object combines component and behavior occurrences under one bound", "[unit][prefab][document]") {
            auto data = Concrete();
            for (std::uint64_t id = 1; id <= PrefabHardLimits::ComponentsPerObject; ++id)
                data.objects.front().components.push_back(Component(id));
            REQUIRE(CreateDocument(data).HasValue());
            data.objects.front().behaviors.push_back(Behavior(1));
            REQUIRE(CreateDocument(data).HasError());

            auto duplicateComponent = Concrete();
            duplicateComponent.objects.front().components = {Component(1), Component(1)};
            REQUIRE(CreateDocument(duplicateComponent).HasError());

            auto duplicateBehavior = Concrete();
            duplicateBehavior.objects.front().behaviors = {Behavior(1), Behavior(1)};
            REQUIRE(CreateDocument(duplicateBehavior).HasError());
        }

        TEST_CASE("Prefab dynamic payload accepts exactly four MiB and rejects one byte more", "[unit][prefab][document]") {
            auto data = Concrete();
            data.objects.front().name.clear();
            const std::size_t typeBytes = ComponentType().Value().size();
            const std::size_t baseChunk = (PrefabHardLimits::SourcePayloadBytes - 4 * typeBytes) / 4;
            const std::size_t remainder = (PrefabHardLimits::SourcePayloadBytes - 4 * typeBytes) % 4;
            for (std::uint64_t id = 1; id <= 4; ++id)
                data.objects.front().components.push_back(Component(id, baseChunk + (id == 4 ? remainder : 0)));
            REQUIRE(CreateDocument(data).HasValue());
            data.objects.front().components.back().component.payload.push_back(std::byte{});
            REQUIRE(CreateDocument(data).HasError());
        }

        TEST_CASE("Prefab document captures a lower validated project policy transactionally", "[unit][prefab][document]") {
            const auto current = CreateDocument(Concrete());
            REQUIRE(current.HasValue());

            PrefabProjectPolicy policy;
            policy.maximumObjectCount = 1;
            const auto limits = PrefabLimitProfile::Create(policy);
            REQUIRE(limits.HasValue());
            auto replacement = Concrete();
            replacement.objects.push_back({.localId = {1}, .parentLocalId = LocalObjectId{}});

            const auto rejected = PrefabDocument::Create(std::move(replacement), limits.Value());
            REQUIRE(rejected.HasError());
            REQUIRE(rejected.ErrorValue().code.Value() == PrefabErrors::ObjectCountExceeded.code.Value());
            REQUIRE(current.Value().Data().objects.size() == 1);
        }

        TEST_CASE("Prefab composition enforces placement identity source revision and declared dependencies", "[unit][prefab][document]") {
            auto data = Concrete();
            data.referencedAssets = {Asset(2)};
            data.composition = PrefabComposition{
                .nestedPlacements = {{.placementLocalId = {1},
                                      .sourcePrefab = PrefabAssetReference::Create(Asset(2)).Value(),
                                      .authoredAgainst = Revision()}},
            };
            REQUIRE(CreateDocument(data).HasValue());

            auto colliding = data;
            colliding.composition->nestedPlacements.front().placementLocalId = {};
            REQUIRE(CreateDocument(colliding).HasError());
            auto undeclared = data;
            undeclared.referencedAssets.clear();
            REQUIRE(CreateDocument(undeclared).HasError());
            auto selfReference = data;
            selfReference.composition->nestedPlacements.front().sourcePrefab = PrefabAssetReference::Create(Asset(1)).Value();
            selfReference.referencedAssets = {Asset(1)};
            REQUIRE(CreateDocument(selfReference).HasError());
        }

        TEST_CASE("Prefab direct placement and asset reference counts are independently bounded", "[unit][prefab][document]") {
            auto placements = Concrete();
            placements.referencedAssets = {Asset(2)};
            placements.composition = PrefabComposition{};
            for (std::uint32_t id = 1; id <= PrefabHardLimits::DirectNestedPlacements; ++id)
                placements.composition->nestedPlacements.push_back({.placementLocalId = {id},
                                                                    .sourcePrefab = PrefabAssetReference::Create(Asset(2)).Value(),
                                                                    .authoredAgainst = Revision()});
            REQUIRE(CreateDocument(placements).HasValue());
            placements.composition->nestedPlacements.push_back(
                {.placementLocalId = {999}, .sourcePrefab = PrefabAssetReference::Create(Asset(2)).Value(), .authoredAgainst = Revision()});
            REQUIRE(CreateDocument(placements).HasError());

            auto references = Concrete();
            for (std::uint16_t id = 2; id < PrefabHardLimits::ReferencedAssets + 2; ++id)
                references.referencedAssets.push_back(Asset(id));
            REQUIRE(CreateDocument(references).HasValue());
            references.referencedAssets.push_back(Asset(500));
            REQUIRE(CreateDocument(references).HasError());
        }

        TEST_CASE("Prefab variants own one exclusive exact parent source", "[unit][prefab][document]") {
            PrefabDocumentData variant{
                .projectVersion = ProjectVersion(),
                .assetId = Asset(1),
                .composition = PrefabComposition{.variantParent = PrefabAssetReference::Create(Asset(2)).Value(),
                                                 .variantAuthoredAgainst = Revision()},
                .referencedAssets = {Asset(2)},
            };
            REQUIRE(CreateDocument(variant).HasValue());

            auto withHierarchy = variant;
            withHierarchy.objects = {Root()};
            REQUIRE(CreateDocument(withHierarchy).HasError());
            auto missingRevision = variant;
            missingRevision.composition->variantAuthoredAgainst.reset();
            REQUIRE(CreateDocument(missingRevision).HasError());
            auto emptyComposition = Concrete();
            emptyComposition.composition = PrefabComposition{};
            REQUIRE(CreateDocument(emptyComposition).HasError());
        }

        TEST_CASE("Prefab document validation rejects noncanonical metadata and leaves source candidates untouched",
                  "[unit][prefab][document]") {
            auto invalidVersion = Concrete();
            invalidVersion.projectVersion.prerelease = std::string(80, 'x');
            const auto original = invalidVersion;
            REQUIRE(CreateDocument(invalidVersion).HasError());
            REQUIRE(invalidVersion == original);

            auto invalidAsset = Concrete();
            invalidAsset.assetId = {};
            REQUIRE(CreateDocument(invalidAsset).HasError());

            auto invalidTransform = Concrete();
            invalidTransform.objects.front().localTransform.translation.x = std::numeric_limits<float>::quiet_NaN();
            REQUIRE(CreateDocument(invalidTransform).HasError());

            auto oversizedName = Concrete();
            oversizedName.objects.front().name.assign(MaximumPrefabObjectNameBytes + 1, 'x');
            REQUIRE(CreateDocument(oversizedName).HasError());

            auto invalidUtf8 = Concrete();
            invalidUtf8.objects.front().name = std::string{"\xC3\x28", 2};
            REQUIRE(CreateDocument(invalidUtf8).HasError());

            auto validUtf8 = Concrete();
            validUtf8.objects.front().name = std::string{"\xF0\x9F\x8C\x8D", 4};
            REQUIRE(CreateDocument(validUtf8).HasValue());
        }

        TEST_CASE("Prefab provider inspection preserves unavailable component behavior and asset payloads") {
            auto data = ProviderDocumentData({Behavior(2)}, 3);
            data.objects.front().components.front().component.payload = {std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}};
            auto document = CreateDocument(data);
            REQUIRE(document.HasValue());
            const PrefabDocumentData originalDocument = document.Value().Data();
            Gameplay::SerializedGameAsset gameAsset = GameAsset();
            const Gameplay::SerializedGameAsset originalAsset = gameAsset;

            Gameplay::ComponentRegistry missingComponents = ComponentProviders();
            Gameplay::GameAssetTypeRegistry missingAssets = GameAssetProviders();
            const Assets::AssetRegistrySnapshot assetProviders = AssetProviders();
            const std::array referencedAssets{PrefabReferencedGameAsset{Asset(2), &gameAsset}};

            const auto missing = document.Value().InspectProviders(assetProviders, missingComponents, {}, missingAssets, referencedAssets);
            REQUIRE(missing.HasValue());
            REQUIRE(missing.Value().IsDegraded());
            REQUIRE(missing.Value().components.front().status == PrefabProviderStatus::Missing);
            REQUIRE(missing.Value().behaviors.front().status == PrefabProviderStatus::Missing);
            REQUIRE(missing.Value().assets.front().status == PrefabProviderStatus::Missing);
            REQUIRE(document.Value().Data() == originalDocument);
            REQUIRE(gameAsset == originalAsset);

            const auto absentAssetPayloads = document.Value().InspectProviders(assetProviders, missingComponents, {}, missingAssets);
            REQUIRE(absentAssetPayloads.HasValue());
            REQUIRE(absentAssetPayloads.Value().IsDegraded());
            REQUIRE(absentAssetPayloads.Value().assets.front().status == PrefabProviderStatus::Missing);
            REQUIRE(absentAssetPayloads.Value().assets.front().type.has_value());

            Gameplay::ComponentRegistry restoredComponents = ComponentProviders(1);
            Gameplay::GameAssetTypeRegistry restoredAssets = GameAssetProviders(1);
            const std::array behaviorDescriptors{BehaviorDescriptor()};

            const auto restored = document.Value().InspectProviders(assetProviders, restoredComponents, behaviorDescriptors, restoredAssets,
                                                                    referencedAssets);
            REQUIRE(restored.HasValue());
            REQUIRE_FALSE(restored.Value().IsDegraded());
            REQUIRE(restored.Value().components.front().status == PrefabProviderStatus::Current);
            REQUIRE(restored.Value().behaviors.front().status == PrefabProviderStatus::Current);
            REQUIRE(restored.Value().assets.front().status == PrefabProviderStatus::Current);
            REQUIRE(document.Value().Data() == originalDocument);
            REQUIRE(gameAsset == originalAsset);
        }

        TEST_CASE("Prefab provider inspection rejects schema skew ambiguous behavior and foreign asset input") {
            auto data = ProviderDocumentData({Behavior(2), Behavior(3)});
            auto document = CreateDocument(data);
            REQUIRE(document.HasValue());

            Gameplay::ComponentDescriptor migratedDescriptor = ComponentDescriptor(2);
            migratedDescriptor.migrations = {{.fromSchemaVersion = 1, .toSchemaVersion = 2}};
            Gameplay::ComponentRegistry components;
            REQUIRE(components.Register(std::move(migratedDescriptor)).HasValue());
            REQUIRE(components.Freeze().HasValue());
            Gameplay::GameAssetTypeRegistry assets = GameAssetProviders(2);
            const std::array behaviorDescriptors{BehaviorDescriptor(2)};
            Gameplay::SerializedGameAsset gameAsset = GameAsset();
            const Assets::AssetRegistrySnapshot assetProviders = AssetProviders();
            const std::array referencedAssets{PrefabReferencedGameAsset{Asset(2), &gameAsset}};

            const auto incompatible =
                document.Value().InspectProviders(assetProviders, components, behaviorDescriptors, assets, referencedAssets);
            REQUIRE(incompatible.HasValue());
            REQUIRE(incompatible.Value().IsDegraded());
            REQUIRE(incompatible.Value().components.front().status == PrefabProviderStatus::MigrationRequired);
            REQUIRE(incompatible.Value().behaviors.front().status == PrefabProviderStatus::IncompatibleSchema);
            REQUIRE(incompatible.Value().behaviors.back().status == PrefabProviderStatus::IncompatibleSchema);
            REQUIRE(incompatible.Value().assets.front().status == PrefabProviderStatus::IncompatibleSchema);

            const std::array duplicateBehaviors{BehaviorDescriptor(), BehaviorDescriptor()};
            REQUIRE(document.Value().InspectProviders(assetProviders, components, duplicateBehaviors, assets, referencedAssets).HasError());
            Gameplay::BehaviorDescriptor invalidBehavior = BehaviorDescriptor();
            invalidBehavior.schemaVersion = 0;
            const std::array invalidBehaviors{invalidBehavior};
            REQUIRE(document.Value().InspectProviders(assetProviders, components, invalidBehaviors, assets, referencedAssets).HasError());

            const std::array foreignAsset{PrefabReferencedGameAsset{Asset(3), &gameAsset}};
            REQUIRE(document.Value().InspectProviders(assetProviders, components, behaviorDescriptors, assets, foreignAsset).HasError());
        }
    }  // namespace
}  // namespace Horo::Prefab
