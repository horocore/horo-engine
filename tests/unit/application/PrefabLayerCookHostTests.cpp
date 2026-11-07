#include "Horo/Prefab/PrefabValueOverrides.h"
#include "PrefabSceneCookHostFixture.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Application;
    using namespace Application::PrefabCookTestSupport;

    /** @brief Keeps actual native cook preparation errors visible before testing publication behavior. */
    template <class T> T Value(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "Actual canonical operation accepted."));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Retains canonical sources/revisions and typed delta intent used by the actual project cook host. */
    struct LayerSource final {
        Assets::AssetId base;
        Prefab::PrefabDocument document;
        std::string encoded;
    };

    /** @brief Creates real persisted base/variant files; transform-only edits require no invented project component policy. */
    LayerSource Prepare(HostFixture &fixture) {
        const auto limits = Value(Prefab::PrefabLimitProfile::Create({}));
        const auto base = fixture.AddSpawnableRoot();
        auto document = Value(
            Prefab::PrefabDocument::Create({.projectVersion = fixture.version,
                                            .assetId = base,
                                            .objects = {{.localId = {0}, .name = "Root"},
                                                        {.localId = {3}, .parentLocalId = Prefab::LocalObjectId{0}, .name = "Child"}}},
                                           limits));
        const auto canonical = Value(document.SerializeCanonical());
        fixture.WriteText("assets/second.prefab", canonical);
        const Prefab::PrefabSourceRevision revision{fixture.version, ComputeSha256(std::as_bytes(std::span{canonical}))};
        auto sources = Value(Prefab::BuildPrefabSourceResolverSnapshot(fixture.registry.Snapshot(), {{document, revision}}, limits));
        auto source = Value(sources.Resolve(base, Prefab::PrefabInstanceId::Create(7).Value(), limits));
        const auto root = Value(Prefab::PrefabObjectAddress::Create({}, {0}));
        const auto child = Value(Prefab::PrefabObjectAddress::Create({}, {3}));
        auto first =
            Value(Prefab::PreparePrefabValueEdit(source, {}, {Prefab::PrefabValueEditKind::Assign, root, Math::Transform{{1, 0, 0}}},
                                                 nullptr, limits));
        auto second = Value(Prefab::PreparePrefabValueEdit(source, first.rebased,
                                                           {Prefab::PrefabValueEditKind::Assign, child, Math::Transform{{0, 2, 0}}},
                                                           nullptr, limits));
        auto variant =
            Value(Prefab::PrefabDocument::Create({.projectVersion = fixture.version,
                                                  .assetId = fixture.prefabId,
                                                  .composition = Prefab::PrefabComposition{.variantParent = Value(
                                                                                               Prefab::PrefabAssetReference::Create(base)),
                                                                                           .variantAuthoredAgainst = revision,
                                                                                           .variantOverrides = second.rebased},
                                                  .referencedAssets = {base}},
                                                 limits));
        auto encoded = Value(variant.SerializeCanonical());
        fixture.WriteText("assets/hierarchy.prefab", encoded);
        fixture.request.runtimePrefabRoots = {fixture.prefabId};
        return {base, std::move(document), std::move(encoded)};
    }

    /** @brief Reads real published template payload bytes instead of comparing cache counters or synthetic expansion results. */
    std::vector<std::uint8_t> TemplateBytes(const HostFixture &fixture, const Assets::AssetCookGeneration &generation) {
        const auto contents = Value(Assets::ReadCookGenerationContents(NativeDurableFileSystem{}, generation, 1024U * 1024U));
        for (const auto &bytes : contents.artifacts) {
            const auto artifact = Value(Assets::DecodeCookedArtifact(bytes));
            if (artifact.id == fixture.prefabId)
                return artifact.payload;
        }
        FAIL("Actual selected runtime template was absent from the published generation.");
        return {};
    }
}  // namespace

TEST_CASE("Actual scene and template cook normalizes layer record order but preserves strict lexical and revision admission",
          "[native][prefab-cook][host][layer]") {
    HostFixture fixture;
    const auto layer = Prepare(fixture);
    INFO("Canonical layered source cook");
    const auto first = Value(fixture.Cook());
    const auto expected = TemplateBytes(fixture, first.generation);
    auto wire = nlohmann::ordered_json::parse(layer.encoded);
    std::ranges::reverse(wire["composition"]["variantOverrides"]["records"]);
    const auto permutation = wire.dump(2) + "\n";
    REQUIRE(ComputeSha256(std::as_bytes(std::span{permutation})) != ComputeSha256(std::as_bytes(std::span{layer.encoded})));
    const auto limits = Value(Prefab::PrefabLimitProfile::Create({}));
    REQUIRE(Value(Value(Prefab::PrefabDocument::Parse(permutation, limits)).MatchesCanonicalInput(permutation)));
    fixture.WriteText("assets/hierarchy.prefab", permutation);
    INFO("Same semantic source under distinct actual raw record-array bytes");
    const auto reordered = Value(fixture.Cook());
    CHECK(TemplateBytes(fixture, reordered.generation) == expected);
    SECTION("noncanonical scalar/source spelling") {
        fixture.WriteText("assets/hierarchy.prefab", wire.dump(2) + "\n ");
    }
    SECTION("unknown record fields") {
        wire["composition"]["variantOverrides"]["records"][0]["extra"] = true;
        fixture.WriteText("assets/hierarchy.prefab", wire.dump(2) + "\n");
    }
    SECTION("source revision changed without accepted rebase") {
        auto changed = layer.document.Data();
        changed.objects[0].localTransform.translation.x = 9;
        const auto document = Value(Prefab::PrefabDocument::Create(std::move(changed), Value(Prefab::PrefabLimitProfile::Create({}))));
        fixture.WriteText("assets/second.prefab", Value(document.SerializeCanonical()));
    }
    const auto rejected = fixture.Cook();
    REQUIRE(rejected.HasError());
    fixture.AssertRetained(reordered.generation);
}
