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

        PrefabObjectNode Root() {
            return {.localId = {}, .parentLocalId = std::nullopt, .name = "Root"};
        }

        PrefabDocumentData Concrete() {
            return {.projectVersion = ProjectVersion(), .assetId = Asset(1), .objects = {Root()}};
        }

        PrefabLimitProfile Limits() {
            return PrefabLimitProfile::Create({}).Value();
        }

        Result<PrefabDocument> CreateDocument(PrefabDocumentData data) {
            return PrefabDocument::Create(std::move(data), Limits());
        }

        TEST_CASE("Prefab document publishes one immutable ordered portable candidate", "[unit][prefab][document]") {
            auto data = Concrete();
            data.referencedAssets = {Asset(2)};
            data.objects.front().components = {Component(1)};
            data.objects.front().behaviors = {Behavior(2)};
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
    }  // namespace
}  // namespace Horo::Prefab
