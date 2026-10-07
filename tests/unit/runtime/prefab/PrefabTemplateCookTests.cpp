#include "Horo/Prefab/PrefabSceneExpansion.h"
#include "Horo/Prefab/PrefabTemplateCook.h"
#include "PrefabTestUtils.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <format>

namespace Horo::Prefab {
    namespace {
        PrefabLimitProfile Profile(const PrefabProjectPolicy &policy = {}) {
            return PrefabLimitProfile::Create(policy).Value();
        }

        AssetCookTargetId Target() {
            return AssetCookTargetId::Parse("headless-null").Value();
        }

        Assets::AssetTypeId Type(std::string_view name) {
            return Assets::AssetTypeId::Parse(name).Value();
        }

        Assets::AssetRecord Record(std::uint16_t id, std::string_view type) {
            const auto path = std::format("assets/item{}{}", id, type == "core.prefab" ? ".prefab" : ".obj");
            return {Test::Asset(id), Type(type), ProjectPath::Parse(path).Value(), ProjectPath::Parse(path + ".horo").Value()};
        }

        PrefabDependencySource Source(std::uint16_t id, std::vector<PrefabObjectNode> objects,
                                      std::optional<PrefabComposition> composition = {}, std::vector<Assets::AssetId> resources = {}) {
            auto document = PrefabDocument::Create({.projectVersion = Application::ParseHoroVersion("1.2.3").Value(),
                                                    .assetId = Test::Asset(id),
                                                    .objects = std::move(objects),
                                                    .composition = std::move(composition),
                                                    .referencedAssets = std::move(resources)},
                                                   Profile());
            REQUIRE(document.HasValue());
            const auto bytes = document.Value().SerializeCanonical().Value();
            PrefabSourceRevision revision{document.Value().Data().projectVersion,
                                          ComputeSha256(std::as_bytes(std::span(bytes.data(), bytes.size())))};
            return {std::move(document).Value(), revision};
        }

        std::vector<std::uint8_t> Envelope(Assets::AssetId asset = Test::Asset(3), AssetCookTargetId target = Target()) {
            const std::vector<std::uint8_t> bytes{1, 2, 3};
            auto artifact = Assets::EncodeCookedArtifact({.id = asset,
                                                          .type = Type("core.mesh"),
                                                          .target = std::move(target),
                                                          .payloadDigest = ComputeSha256(std::as_bytes(std::span(bytes))),
                                                          .payload = bytes});
            REQUIRE(artifact.HasValue());
            return std::move(artifact).Value();
        }

        struct Fixture final {
            Assets::AssetRegistry registry;
            std::vector<std::uint8_t> resource{Envelope()};
            PrefabSourceResolverSnapshot sources;

            Fixture() : sources(Build()) {}

            PrefabSourceResolverSnapshot Build() {
                REQUIRE(registry.Publish({Record(1, "core.prefab"), Record(2, "core.prefab"), Record(3, "core.mesh")}).status ==
                        Assets::AssetRegistryBuildStatus::Complete);
                PrefabObjectNode child{.localId = {0}, .name = "Nested"};
                child.localTransform.translation = {1, 2, 3};
                auto nested = Source(2, {child}, {}, {Test::Asset(3)});
                PrefabComposition composition{.nestedPlacements = {{.placementLocalId = {7},
                                                                    .sourcePrefab = PrefabAssetReference::Create(Test::Asset(2)).Value(),
                                                                    .authoredAgainst = nested.sourceRevision}}};
                auto outer = Source(1, {{.localId = {0}, .name = "Root"}}, composition, {Test::Asset(2)});
                auto snapshot = BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {outer, nested}, Profile());
                REQUIRE(snapshot.HasValue());
                return std::move(snapshot).Value();
            }

            Result<CookedPrefab> Cook(const PrefabProjectPolicy &policy = {}, const CancellationToken &cancellation = {}) const {
                const PrefabTemplateCookResource dependency{Test::Asset(3), resource};
                return CookPrefabTemplate(sources, registry.Snapshot(), Test::Asset(), std::span{&dependency, 1}, Target(), Profile(policy),
                                          {cancellation});
            }
        };
    }  // namespace

    TEST_CASE("Template cook uses scene resolver transforms topology and source identities", "[prefab][template-cook]") {
        Fixture fixture;
        auto cooked = fixture.Cook();
        REQUIRE(cooked.HasValue());
        const auto &data = cooked.Value().Data();
        REQUIRE(data.entities.size() == 2);
        CHECK(data.entities[1].parent == CookedPrefabEntitySlot{0});
        CHECK(data.entities[1].provenance.sourceAsset == Test::Asset(2));
        CHECK(data.entities[1].provenance.sourceObject.NestedInstanceScope().front() == LocalObjectId{7});
        REQUIRE(data.dependencies.size() == 1);
        CHECK(data.dependencies[0].asset.id == Test::Asset(3));
        CHECK(data.dependencies[0].artifactDigest == ComputeSha256(std::as_bytes(std::span(fixture.resource))));
        auto candidate = fixture.sources.Resolve(Test::Asset(), PrefabInstanceId::Create(1).Value(), Profile()).Value();
        auto remap = RemapPrefabCandidateToScene(candidate, {}, {}, Profile());
        REQUIRE(remap.HasValue());
        std::vector<PrefabRuntimeComponentProjection> projections;
        for (const auto &object : candidate.Objects())
            projections.push_back({object.key, {}});
        auto scene = ExpandPrefabSceneSubtree(candidate, remap.Value(), projections, {}, Profile());
        REQUIRE(scene.HasValue());
        for (std::size_t index = 0; index < data.entities.size(); ++index)
            CHECK(data.entities[index].localTransform == scene.Value().Entities()[index].localTransform);
        auto repeated = fixture.Cook();
        REQUIRE(repeated.HasValue());
        CHECK(std::ranges::equal(cooked.Value().Bytes(), repeated.Value().Bytes()));
        CHECK(CookedPrefab::Parse(cooked.Value().Bytes(), Test::Asset(), Profile()).HasValue());
    }

    TEST_CASE("Template cook rejects incomplete foreign and duplicate resource closure", "[prefab][template-cook]") {
        Fixture fixture;
        std::vector<PrefabTemplateCookResource> resources;
        SECTION("missing") {
            // Leave the captured resource closure empty to verify missing-resource rejection.
        }
        SECTION("foreign") {
            resources.emplace_back(Test::Asset(2), fixture.resource);
        }
        SECTION("duplicate") {
            resources.emplace_back(Test::Asset(3), fixture.resource);
            resources.push_back(resources.front());
        }
        auto result = CookPrefabTemplate(fixture.sources, fixture.registry.Snapshot(), Test::Asset(), resources, Target(), Profile());
        CHECK(result.HasError());
    }

    TEST_CASE("Template cook verifies actual resource envelopes before recording dependencies", "[prefab][template-cook]") {
        Fixture fixture;
        SECTION("digest corruption") {
            std::as_writable_bytes(std::span{fixture.resource}).back() ^= std::byte{1};
        }
        SECTION("truncation") {
            fixture.resource.resize(7);
        }
        SECTION("foreign identity") {
            fixture.resource = Envelope(Test::Asset(2));
        }
        SECTION("wrong target") {
            fixture.resource = Envelope(Test::Asset(3), AssetCookTargetId::Parse("linux-x64").Value());
        }
        SECTION("wrong type") {
            auto envelope = Assets::DecodeCookedArtifact(fixture.resource).Value();
            envelope.type = Type("core.texture");
            fixture.resource = Assets::EncodeCookedArtifact(envelope).Value();
        }
        CHECK(fixture.Cook().HasError());
    }

    TEST_CASE("Template cook canonicalizes captured resource order", "[prefab][template-cook]") {
        Assets::AssetRegistry registry;
        REQUIRE(registry.Publish({Record(1, "core.prefab"), Record(2, "core.mesh"), Record(3, "core.mesh")}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
        auto snapshot =
            BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                              {Source(1, {{.localId = {0}, .name = "Root"}}, {}, {Test::Asset(2), Test::Asset(3)})},
                                              Profile());
        REQUIRE(snapshot.HasValue());
        const auto first = Envelope(Test::Asset(2));
        const auto second = Envelope(Test::Asset(3));
        std::vector<PrefabTemplateCookResource> resources{{Test::Asset(2), first}, {Test::Asset(3), second}};
        auto canonical = CookPrefabTemplate(snapshot.Value(), registry.Snapshot(), Test::Asset(), resources, Target(), Profile());
        REQUIRE(canonical.HasValue());
        std::ranges::reverse(resources);
        auto reordered = CookPrefabTemplate(snapshot.Value(), registry.Snapshot(), Test::Asset(), resources, Target(), Profile());
        REQUIRE(reordered.HasValue());
        CHECK(std::ranges::equal(canonical.Value().Bytes(), reordered.Value().Bytes()));
    }

    TEST_CASE("Template cook observes lowered limits and cancellation without changing a prior template", "[prefab][template-cook]") {
        Fixture fixture;
        auto original = fixture.Cook();
        REQUIRE(original.HasValue());
        const std::vector<std::byte> saved(original.Value().Bytes().begin(), original.Value().Bytes().end());
        PrefabProjectPolicy policy;
        policy.maximumObjectCount = 2;
        policy.maximumHierarchyDepth = 2;
        CHECK(fixture.Cook(policy).HasValue());
        policy.maximumObjectCount = 1;
        CHECK(fixture.Cook(policy).HasError());
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CHECK(fixture.Cook({}, cancelled.Token()).HasError());
        std::as_writable_bytes(std::span{fixture.resource}).back() ^= std::byte{1};
        CHECK(fixture.Cook().HasError());
        CHECK(std::ranges::equal(saved, original.Value().Bytes()));
    }

    TEST_CASE("Template cook rejects a registry replacement before resolution", "[prefab][template-cook]") {
        Fixture fixture;
        REQUIRE(fixture.registry.Publish({Record(1, "core.prefab"), Record(2, "core.prefab"), Record(3, "core.mesh")}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
        const auto result = fixture.Cook();
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == PrefabErrors::ResolutionStale.code.Value());
    }

    TEST_CASE("Template cook retains portable component and behavior schema requirements", "[prefab][template-cook]") {
        Assets::AssetRegistry registry;
        REQUIRE(registry.Publish({Record(1, "core.prefab")}).status == Assets::AssetRegistryBuildStatus::Complete);
        PrefabObjectNode object{.localId = {0}, .name = "Root"};
        RawComponentPayload component;
        component.instance = PrefabComponentInstanceId::Create(11).Value();
        component.component.typeId = Gameplay::ComponentTypeId::Parse("game.sample.health").Value();
        component.component.payload = {std::byte{'{'}, std::byte{'}'}};
        object.components.push_back(component);
        Gameplay::BehaviorComponent behavior;
        behavior.instanceId = {13};
        behavior.typeId = Gameplay::BehaviorTypeId::Parse("game.sample.controller").Value();
        behavior.enabled = false;
        behavior.fields = {{"speed", 2.5}};
        object.behaviors.push_back(behavior);
        auto snapshot = BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {Source(1, {object})}, Profile());
        REQUIRE(snapshot.HasValue());
        auto cooked = CookPrefabTemplate(snapshot.Value(), registry.Snapshot(), Test::Asset(), {}, Target(), Profile());
        REQUIRE(cooked.HasValue());
        const auto &members = cooked.Value().Data().entities[0].members;
        REQUIRE(members.size() == 2);
        CHECK(std::get<RawComponentPayload>(members[0]) == component);
        CHECK(std::get<Gameplay::BehaviorComponent>(members[1]) == behavior);
        const auto parsed = CookedPrefab::Parse(cooked.Value().Bytes(), Test::Asset(), Profile());
        REQUIRE(parsed.HasValue());
        CHECK(parsed.Value().Data() == cooked.Value().Data());
    }

    TEST_CASE("Template cook flattens a variant with the same resolver authority", "[prefab][template-cook]") {
        Assets::AssetRegistry registry;
        REQUIRE(registry.Publish({Record(1, "core.prefab"), Record(2, "core.prefab")}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
        auto base = Source(2, {{.localId = {0}, .name = "Base"}, {.localId = {17}, .parentLocalId = LocalObjectId{0}, .name = "Child"}});
        PrefabComposition composition{.variantParent = PrefabAssetReference::Create(Test::Asset(2)).Value(),
                                      .variantAuthoredAgainst = base.sourceRevision};
        auto variant = Source(1, {}, composition, {Test::Asset(2)});
        auto snapshot = BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {variant, base}, Profile());
        REQUIRE(snapshot.HasValue());
        auto cooked = CookPrefabTemplate(snapshot.Value(), registry.Snapshot(), Test::Asset(), {}, Target(), Profile());
        REQUIRE(cooked.HasValue());
        CHECK(cooked.Value().Data().entities.size() == 2);
        CHECK(cooked.Value().Data().entities[1].provenance.sourceAsset == Test::Asset(2));
        CHECK(cooked.Value().Data().entities[1].provenance.sourceObject.SourceObject() == LocalObjectId{17});
        CHECK(cooked.Value().Data().dependencies.empty());
    }

    TEST_CASE("Template cook binds changed resource envelope bytes to the runtime dependency digest", "[prefab][template-cook]") {
        Fixture fixture;
        auto previous = fixture.Cook();
        REQUIRE(previous.HasValue());
        auto resource = Assets::DecodeCookedArtifact(fixture.resource);
        REQUIRE(resource.HasValue());
        auto artifact = std::move(resource).Value();
        artifact.payload = {4, 5, 6};
        artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span(artifact.payload)));
        fixture.resource = Assets::EncodeCookedArtifact(artifact).Value();
        auto changed = fixture.Cook();
        REQUIRE(changed.HasValue());
        CHECK(previous.Value().Data().dependencies[0].artifactDigest != changed.Value().Data().dependencies[0].artifactDigest);
        CHECK_FALSE(std::ranges::equal(previous.Value().Bytes(), changed.Value().Bytes()));
    }

    TEST_CASE("Template cook rejects missing root and invalid target without output", "[prefab][template-cook]") {
        Fixture fixture;
        const PrefabTemplateCookResource resource{Test::Asset(3), fixture.resource};
        CHECK(CookPrefabTemplate(fixture.sources, fixture.registry.Snapshot(), Test::Asset(9), std::span{&resource, 1}, Target(), Profile())
                  .HasError());
        const auto invalid =
            CookPrefabTemplate(fixture.sources, fixture.registry.Snapshot(), Test::Asset(), std::span{&resource, 1}, {}, Profile());
        REQUIRE(invalid.HasError());
        CHECK(invalid.ErrorValue().code.Value() == AssetCookTargetErrors::Invalid.code.Value());
    }

    TEST_CASE("Resource envelope limits remain separate from the small flattened template", "[prefab][template-cook]") {
        Fixture fixture;
        auto resource = Assets::DecodeCookedArtifact(fixture.resource).Value();
        resource.payload.resize(16U * 1024U, 42);
        resource.payloadDigest = ComputeSha256(std::as_bytes(std::span(resource.payload)));
        fixture.resource = Assets::EncodeCookedArtifact(resource).Value();
        PrefabProjectPolicy policy;
        policy.maximumCookedPayloadBytes = 1024;
        REQUIRE(fixture.Cook(policy).HasValue());
        Assets::AssetCookLimits resourceLimits;
        resourceLimits.maximumArtifactBytes = fixture.resource.size();
        const PrefabTemplateCookResource dependency{Test::Asset(3), fixture.resource};
        CHECK(CookPrefabTemplate(fixture.sources, fixture.registry.Snapshot(), Test::Asset(), std::span{&dependency, 1}, Target(),
                                 Profile(policy), {{}, resourceLimits})
                  .HasValue());
        --resourceLimits.maximumArtifactBytes;
        CHECK(CookPrefabTemplate(fixture.sources, fixture.registry.Snapshot(), Test::Asset(), std::span{&dependency, 1}, Target(),
                                 Profile(policy), {{}, resourceLimits})
                  .HasError());
    }
}  // namespace Horo::Prefab
