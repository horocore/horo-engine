#include "Horo/Prefab/PrefabExpansionCache.h"
#include "PrefabLayerOverrideTestSupport.h"

#include <limits>

using namespace Horo;

namespace {
    /** @brief Compares every owned diagnostic field, including actionable source/path metadata. */
    void CheckSameDiagnostic(const Diagnostic &a, const Diagnostic &b) {
        CHECK(a.code.Value() == b.code.Value());
        CHECK(a.severity == b.severity);
        CHECK(a.message == b.message);
        CHECK(a.location.source == b.location.source);
        CHECK(a.location.line == b.location.line);
        CHECK(a.location.column == b.location.column);
        CHECK(a.path == b.path);
    }

    /** @brief Proves exact public error identity, presentation metadata and complete cause chains after record permutation. */
    void CheckSameError(const Error &a, const Error &b) {
        CHECK(a.domain.Value() == b.domain.Value());
        CHECK(a.code.Value() == b.code.Value());
        CHECK(a.severity == b.severity);
        CHECK(a.message == b.message);
        REQUIRE(a.diagnostics.size() == b.diagnostics.size());
        for (std::size_t i = 0; i < a.diagnostics.size(); ++i)
            CheckSameDiagnostic(a.diagnostics[i], b.diagnostics[i]);
        REQUIRE((a.cause.Get() == nullptr) == (b.cause.Get() == nullptr));
        if (a.cause.Get() != nullptr)
            CheckSameError(*a.cause.Get(), *b.cause.Get());
    }
}  // namespace

TEST_CASE("Canonical prefab layers apply source variant placement containing variant and scene instance in order",
          "[prefab][override][layer]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    const auto base = ValueTestSupport::Asset();
    const auto variant = Id(92);
    const auto containing = Id(93);
    const auto containingVariant = Id(94);
    graph.AddVariant(92, 91, Revision(1), graph.Assign(base, Property(), 3.0));
    CHECK(Speed(graph.Resolve(variant), Object()) == 3.0);
    PrefabComposition composition{.nestedPlacements = {{.placementLocalId = {7},
                                                        .sourcePrefab = Take(PrefabAssetReference::Create(variant)),
                                                        .authoredAgainst = Revision(92),
                                                        .valueOverrides = graph.Assign(variant, Property(), 4.0)}}};
    auto document = Take(PrefabDocument::Create({.projectVersion = Revision(93).projectVersion,
                                                 .assetId = containing,
                                                 .objects = {{.localId = {0}, .name = "Containing"}},
                                                 .composition = std::move(composition),
                                                 .referencedAssets = {variant}},
                                                Limits()));
    graph.sources.push_back({std::move(document), Revision(93)});
    CHECK(Speed(graph.Resolve(containing), NestedProperty().Object()) == 4.0);
    graph.AddVariant(94, 93, Revision(93), graph.Assign(containing, NestedProperty(), 5.0));
    auto source = graph.Resolve(containingVariant);
    CHECK(Speed(source, NestedProperty().Object()) == 5.0);
    auto instance =
        Take(PreparePrefabValueEdit(source, {}, {PrefabValueEditKind::Assign, NestedProperty(), 6.0}, graph.schemas.get(), Limits()));
    REQUIRE(instance.effective);
    CHECK(Speed(*instance.effective, NestedProperty().Object()) == 6.0);
    CHECK(Speed(graph.Resolve(base), Object()) == 2.0);
    for (const auto &entry : graph.sources) {
        const auto encoded = Take(entry.document.SerializeCanonical());
        const auto parsed = Take(PrefabDocument::Parse(encoded, Limits()));
        CHECK(Take(parsed.SerializeCanonical()) == encoded);
    }
}

TEST_CASE("Combined topology layers remove and add stable component occurrences before later layers", "[prefab][override][layer]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    Gameplay::ComponentDescriptor descriptor{Type(),
                                             1,
                                             "Values",
                                             "Tests",
                                             {{Gameplay::ComponentPropertyId::Parse("speed").Value(), "Speed",
                                               Gameplay::ComponentPropertyKind::Number, true}},
                                             {}};
    const auto values = Schemas();
    auto properties = std::vector<PrefabValuePropertySchema>{values.Properties().begin(), values.Properties().end()};
    properties[0].required = true;
    graph.schemas = std::make_shared<const PrefabValueSchemaSnapshot>(
        Take(PrefabValueSchemaSnapshot::Create(std::move(properties), Limits(), {{std::move(descriptor), 1, true, true, {}, {}, {}}})));
    const auto original = graph.Resolve(Asset());
    RawComponentPayload replacement{PrefabComponentInstanceId::Create(2).Value(), {.typeId = Type()}};
    const std::string text = "{\"speed\":3.0}";
    replacement.component.payload.assign(reinterpret_cast<const std::byte *>(text.data()),
                                         reinterpret_cast<const std::byte *>(text.data() + text.size()));
    const PrefabComponentOverrideTarget first{Object(), Type(), PrefabComponentInstanceId::Create(1).Value()};
    auto changed =
        Take(PreparePrefabComponentEdit(original, {}, PrefabReplaceComponentEdit{first, replacement}, graph.schemas.get(), Limits()));
    graph.AddVariant(92, 91, Revision(1), changed.rebased);
    const auto variant = graph.Resolve(Id(92));
    REQUIRE(variant.Objects()[0].object.components == std::vector{replacement});
    const PrefabComponentOverrideTarget second{Object(), Type(), replacement.instance};
    auto nestedReplacement = replacement;
    nestedReplacement.instance = PrefabComponentInstanceId::Create(3).Value();
    auto removed =
        Take(PreparePrefabComponentEdit(variant, {}, PrefabReplaceComponentEdit{second, nestedReplacement}, graph.schemas.get(), Limits()));
    PrefabComposition composition{.nestedPlacements = {{.placementLocalId = {7},
                                                        .sourcePrefab = Take(PrefabAssetReference::Create(Id(92))),
                                                        .authoredAgainst = Revision(92),
                                                        .valueOverrides = removed.rebased}}};
    auto containing = Take(PrefabDocument::Create({.projectVersion = Revision(93).projectVersion,
                                                   .assetId = Id(93),
                                                   .objects = {{.localId = {0}, .name = "Host"}},
                                                   .composition = std::move(composition),
                                                   .referencedAssets = {Id(92)}},
                                                  Limits()));
    graph.sources.push_back({std::move(containing), Revision(93)});
    const auto output = graph.Resolve(Id(93));
    REQUIRE(output.Objects().size() == 2);
    CHECK(output.Objects()[1].object.components == std::vector{nestedReplacement});
    const PrefabComponentOverrideTarget nested{NestedProperty().Object(), Type(), nestedReplacement.instance};
    auto finalRemoval = Take(PreparePrefabComponentEdit(output, {}, PrefabRemoveComponentEdit{nested}, graph.schemas.get(), Limits()));
    graph.AddVariant(94, 93, Revision(93), finalRemoval.rebased);
    const auto finalSource = graph.Resolve(Id(94));
    CHECK(finalSource.Objects()[1].object.components.empty());
    auto scenePayload = replacement;
    scenePayload.instance = PrefabComponentInstanceId::Create(4).Value();
    const auto scene = Take(PreparePrefabComponentEdit(finalSource, {}, PrefabAddComponentOverride{NestedProperty().Object(), scenePayload},
                                                       graph.schemas.get(), Limits()));
    REQUIRE(scene.effective);
    CHECK(scene.effective->Objects()[1].object.components == std::vector{scenePayload});
    CHECK(graph.Resolve(Asset()).Objects()[0].object.components[0].instance == first.instance);
}

TEST_CASE("Record array arrival order alone is normalized while canonical scalar and lexical admission stays strict",
          "[prefab][override][codec]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    const auto source = graph.Resolve(Asset());
    auto set = graph.Assign(Asset(), Property(), 3.0);
    auto transform = Take(PreparePrefabValueEdit(source, set, {PrefabValueEditKind::Assign, Object(), Math::Transform{{1, 2, 3}}},
                                                 graph.schemas.get(), Limits()));
    const auto canonical = Take(SerializePrefabValueOverrides(transform.rebased, Limits()));
    auto wire = nlohmann::json::parse(canonical);
    std::ranges::reverse(wire["records"]);
    const auto reordered = wire.dump(0);
    CHECK(Take(SerializePrefabValueOverrides(Take(ParsePrefabValueOverrides(reordered, Limits())), Limits())) == canonical);
    SECTION("numeric lexeme") {
        auto bytes = canonical;
        const auto position = bytes.find("3.0");
        REQUIRE(position != std::string::npos);
        bytes.replace(position, 3, "3e0");
        CHECK(ParsePrefabValueOverrides(bytes, Limits()).HasError());
    }
    SECTION("whitespace") {
        CHECK(ParsePrefabValueOverrides(canonical + " ", Limits()).HasError());
    }
    SECTION("unknown row field") {
        wire["records"][0]["unexpected"] = true;
        CHECK(ParsePrefabValueOverrides(wire.dump(0), Limits()).HasError());
    }
    SECTION("escaped spelling") {
        set.records[0].value = std::string("abc");
        auto bytes = Take(SerializePrefabValueOverrides(set, Limits()));
        const auto position = bytes.find("abc");
        REQUIRE(position != std::string::npos);
        bytes.replace(position, 3, "\\u0061bc");
        CHECK(ParsePrefabValueOverrides(bytes, Limits()).HasError());
    }
    SECTION("duplicate semantic target") {
        wire["records"].push_back(wire["records"][0]);
        const auto first = ParsePrefabValueOverrides(wire.dump(0), Limits());
        std::ranges::reverse(wire["records"]);
        const auto second = ParsePrefabValueOverrides(wire.dump(0), Limits());
        REQUIRE(first.HasError());
        REQUIRE(second.HasError());
        CHECK(first.ErrorValue().code.Value() == second.ErrorValue().code.Value());
        CHECK(first.ErrorValue().message == second.ErrorValue().message);
    }
}

TEST_CASE("Authoring stale source capture preserves conflict evidence but strict resolution and cached publication remain closed",
          "[prefab][override][revision]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    graph.AddVariant(92, 91, Revision(1), graph.Assign(Asset(), Property(), 3.0));
    auto accepted = graph.Resolve(Id(92));
    const auto oldRevision = accepted.Revision();
    graph.sources[0] = ValueTestSupport::Sources("{\"speed\":4.0}", 2).Sources()[0];
    REQUIRE(BuildPrefabSourceResolverSnapshot(graph.registry.Snapshot(), graph.sources, Limits(), {}, graph.schemas).HasError());
    auto authoring = graph.Snapshot(PrefabSourceRevisionPolicy::PreserveLayerRebase);
    const auto outcome = Take(authoring.ResolveOverrides(Id(92), PrefabInstanceId::Create(9).Value(), Limits()));
    CHECK_FALSE(outcome.effective);
    REQUIRE(outcome.layers.size() == 1);
    REQUIRE(outcome.layers[0].rebased.conflicts.size() == 1);
    CHECK(outcome.layers[0].rebased.conflicts[0].authoredAgainst == Revision(1));
    CHECK(outcome.layers[0].rebased.conflicts[0].observedSource == Revision(2));
    CHECK(authoring.Resolve(Id(92), PrefabInstanceId::Create(9).Value(), Limits()).HasError());
    PrefabExpansionCache cache;
    CHECK(cache.Resolve(authoring, Id(92), PrefabInstanceId::Create(9).Value(), Limits()).HasError());
    CHECK(Speed(accepted, Object()) == 3.0);
    CHECK(accepted.Revision() == oldRevision);
}

TEST_CASE("Missing exact schemas retain layer intent as orphans and reject runtime candidates", "[prefab][override][schema]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    graph.AddVariant(92, 91, Revision(1), graph.Assign(Asset(), Property(), 3.0));
    graph.schemas.reset();
    const auto snapshot = graph.Snapshot();
    const auto result = Take(snapshot.ResolveOverrides(Id(92), PrefabInstanceId::Create(9).Value(), Limits()));
    REQUIRE(result.layers.size() == 1);
    CHECK_FALSE(result.effective);
    REQUIRE(result.layers[0].rebased.orphans.size() == 1);
    CHECK(result.layers[0].rebased.orphans[0].reason == PrefabValueOverrideIssue::SchemaUnavailable);
    CHECK(snapshot.Resolve(Id(92), PrefabInstanceId::Create(9).Value(), Limits()).HasError());
}

TEST_CASE("Variant preflight admits the exact eight-edge boundary and rejects the next edge without changing accepted output",
          "[prefab][override][boundary]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    graph.AddParent(92, 91, Revision(1));
    for (std::uint8_t owner = 93; owner <= 99; ++owner)
        graph.AddParent(owner, owner - 1, Revision(owner - 1));
    const auto accepted = graph.Resolve(Id(99));
    CHECK(Speed(accepted, Object()) == 2.0);
    graph.AddParent(100, 99, Revision(99));
    const auto rejected = graph.Snapshot().Resolve(Id(100), PrefabInstanceId::Create(9).Value(), Limits());
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::HierarchyDepthExceeded.code.Value());
    CHECK(Speed(accepted, Object()) == 2.0);
}

TEST_CASE("Mixed dependency cycles are rejected before any source expansion and cancellation leaves accepted values intact",
          "[prefab][override][lifecycle]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    const auto accepted = graph.Resolve(Asset());
    SECTION("cycle") {
        graph.AddParent(92, 93, Revision(93));
        graph.AddNested(93, 92, Revision(92));
        const auto rejected = graph.Snapshot().Resolve(Id(92), PrefabInstanceId::Create(9).Value(), Limits());
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::DependencyGraphInvalid.code.Value());
    }
    SECTION("cancel") {
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        const auto rejected = graph.Snapshot().Resolve(Asset(), PrefabInstanceId::Create(9).Value(), Limits(), cancellation.Token());
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::Cancelled.code.Value());
    }
    CHECK(Speed(accepted, Object()) == 2.0);
}

TEST_CASE("Resolver cache identity includes actual schema contracts even when source bytes and metadata are identical",
          "[prefab][override][cache]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    graph.AddVariant(92, 91, Revision(1), graph.Assign(Asset(), Property(), 3.0));
    const auto first = graph.Snapshot();
    PrefabExpansionCache cache;
    const auto firstKey = Take(cache.CaptureKey(first, Id(92), PrefabInstanceId::Create(9).Value(), Limits()));
    auto properties = std::vector<PrefabValuePropertySchema>{graph.schemas->Properties().begin(), graph.schemas->Properties().end()};
    properties[0].maximum = 9.0;
    graph.schemas =
        std::make_shared<const PrefabValueSchemaSnapshot>(Take(PrefabValueSchemaSnapshot::Create(std::move(properties), Limits())));
    const auto second = Take(BuildPrefabSourceResolverSnapshot(graph.registry.Snapshot(), graph.sources, Limits(), {}, graph.schemas));
    CHECK(first.RegistryRevision() == second.RegistryRevision());
    const auto secondKey = Take(cache.CaptureKey(second, Id(92), PrefabInstanceId::Create(9).Value(), Limits()));
    CHECK_FALSE(firstKey == secondKey);
    const auto accepted = Take(cache.Resolve(first, Id(92), PrefabInstanceId::Create(9).Value(), Limits()));
    REQUIRE(cache.Find(firstKey) == accepted);
    CHECK_FALSE(cache.Find(secondKey));
    CHECK(second.ValidateRevisionPublication(Id(92), accepted->Revision(), {}, Limits()).HasError());
    const auto authoring = Take(
        BuildPrefabSourceResolverSnapshot(graph.registry.Snapshot(), graph.sources, Limits(), {},
                                          first.ValueSchemas() ? std::make_shared<const PrefabValueSchemaSnapshot>(*first.ValueSchemas())
                                                               : nullptr,
                                          PrefabSourceRevisionPolicy::PreserveLayerRebase));
    CHECK_FALSE(Take(cache.CaptureKey(authoring, Id(92), PrefabInstanceId::Create(9).Value(), Limits())) == firstKey);
    CHECK(first.ValueSchemas()->Revision() != second.ValueSchemas()->Revision());
}

TEST_CASE("Nested preflight admits sixteen actual edges and rejects seventeen before touching the last accepted hierarchy",
          "[prefab][override][boundary]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    graph.AddNested(110, 91, Revision(1));
    for (std::uint8_t owner = 111; owner <= 125; ++owner)
        graph.AddNested(owner, owner - 1, Revision(owner - 1));
    const auto accepted = graph.Resolve(Id(125));
    CHECK(accepted.Objects().size() == 17);
    graph.AddNested(126, 125, Revision(125));
    const auto rejected = graph.Snapshot().Resolve(Id(126), PrefabInstanceId::Create(9).Value(), Limits());
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::HierarchyDepthExceeded.code.Value());
    CHECK(accepted.Objects().size() == 17);
}

TEST_CASE("Nested placement storage order cannot select a different effective output or malformed-edge diagnostic",
          "[prefab][override][order]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    graph.AddNested(92, 91, Revision(1));
    auto data = graph.sources.back().document.Data();
    auto second = data.composition->nestedPlacements[0];
    second.placementLocalId = {8};
    data.composition->nestedPlacements.insert(data.composition->nestedPlacements.begin(), second);
    graph.sources.back().document = Take(PrefabDocument::Create(data, Limits()));
    const auto first = graph.Resolve(Id(92));
    std::ranges::reverse(data.composition->nestedPlacements);
    graph.sources.back().document = Take(PrefabDocument::Create(data, Limits()));
    const auto secondOutput = graph.Resolve(Id(92));
    CHECK(std::ranges::equal(first.Objects(), secondOutput.Objects()));
    data.composition->nestedPlacements[0].authoredAgainst = Revision(7);
    data.composition->nestedPlacements[1].authoredAgainst = Revision(8);
    graph.sources.back().document = Take(PrefabDocument::Create(data, Limits()));
    const auto rejected = BuildPrefabSourceResolverSnapshot(graph.registry.Snapshot(), graph.sources, Limits(), {}, graph.schemas);
    std::ranges::reverse(data.composition->nestedPlacements);
    graph.sources.back().document = Take(PrefabDocument::Create(data, Limits()));
    const auto reordered = BuildPrefabSourceResolverSnapshot(graph.registry.Snapshot(), graph.sources, Limits(), {}, graph.schemas);
    REQUIRE(rejected.HasError());
    REQUIRE(reordered.HasError());
    CHECK(rejected.ErrorValue().code.Value() == reordered.ErrorValue().code.Value());
    CHECK(rejected.ErrorValue().message == reordered.ErrorValue().message);
}

TEST_CASE("Canonical source admission normalizes only complete layer array order without accepting different source spelling",
          "[prefab][override][source-codec]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    const auto source = graph.Resolve(Asset());
    auto assigned = graph.Assign(Asset(), Property(), 3.0);
    auto combined = Take(PreparePrefabValueEdit(source, assigned, {PrefabValueEditKind::Assign, Object(), Math::Transform{{1, 2, 3}}},
                                                graph.schemas.get(), Limits()));
    graph.AddVariant(92, 91, Revision(1), combined.rebased);
    const auto &document = graph.sources.back().document;
    const auto canonical = Take(document.SerializeCanonical());
    auto wire = nlohmann::ordered_json::parse(canonical);
    std::ranges::reverse(wire["composition"]["variantOverrides"]["records"]);
    const auto permutation = wire.dump(2) + "\n";
    const auto parsed = Take(PrefabDocument::Parse(permutation, Limits()));
    CHECK(Take(parsed.MatchesCanonicalInput(permutation)));
    CHECK(Take(parsed.SerializeCanonical()) == canonical);
    CHECK_FALSE(Take(parsed.MatchesCanonicalInput(permutation + " ")));
    auto changed = permutation;
    const auto position = changed.find("3.0");
    REQUIRE(position != std::string::npos);
    changed.replace(position, 3, "3e0");
    CHECK_FALSE(Take(parsed.MatchesCanonicalInput(changed)));
    wire["composition"]["variantOverrides"]["records"][0]["unknown"] = true;
    CHECK(PrefabDocument::Parse(wire.dump(2) + "\n", Limits()).HasError());
}

TEST_CASE("Lowered operation budgets and duplicate layer intent fail before a replacement hierarchy is exposed",
          "[prefab][override][budget]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    const auto accepted = graph.Resolve(Asset());
    auto set = graph.Assign(Asset(), Property(), 3.0);
    const auto second = Take(PreparePrefabValueEdit(accepted, set, {PrefabValueEditKind::Assign, Object(), Math::Transform{{1, 2, 3}}},
                                                    graph.schemas.get(), Limits()));
    graph.AddVariant(92, 91, Revision(1), second.rebased);
    auto policy = Limits().Policy();
    policy.maximumOverrideRecords = 1;
    const auto lowered = Take(PrefabLimitProfile::Create(policy));
    const auto rejected = graph.Snapshot().Resolve(Id(92), PrefabInstanceId::Create(9).Value(), lowered);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::WorkBudgetExceeded.code.Value());
    auto duplicate = graph.sources.back().document.Data();
    duplicate.composition->variantOverrides->records.push_back(duplicate.composition->variantOverrides->records[0]);
    const auto malformed = PrefabDocument::Create(std::move(duplicate), Limits());
    REQUIRE(malformed.HasError());
    CHECK(malformed.ErrorValue().code.Value() == PrefabErrors::OverrideConflict.code.Value());
    CHECK(Speed(accepted, Object()) == 2.0);
}

TEST_CASE("Nested mounts run once after source variant and placement transforms are resolved", "[prefab][override][mount]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    auto data = graph.sources[0].document.Data();
    data.objects[0].localTransform.rotation = Math::Quaternion::FromAxisAngle({0, 0, 1}, 0.6F);
    graph.sources[0].document = Take(PrefabDocument::Create(std::move(data), Limits()));
    const auto original = graph.Resolve(Asset());
    const auto reset = Take(
        PreparePrefabValueEdit(original, {}, {PrefabValueEditKind::Assign, Object(), Math::Transform{}}, graph.schemas.get(), Limits()));
    auto referenced = Asset();
    std::optional<PrefabValueOverrideSet> layer;
    bool rejects = false;
    SECTION("source variant removes rotation before the containing nonuniform mount") {
        graph.AddVariant(92, 91, Revision(1), reset.rebased);
        referenced = Id(92);
    }
    SECTION("placement removes rotation before its own nonuniform mount") {
        layer = reset.rebased;
    }
    SECTION("genuinely shearing final mount remains rejected") {
        rejects = true;
    }
    const auto revision = referenced == Asset() ? Revision(1) : Revision(92);
    PrefabComposition composition{.nestedPlacements = {{.placementLocalId = {7},
                                                        .sourcePrefab = Take(PrefabAssetReference::Create(referenced)),
                                                        .authoredAgainst = revision,
                                                        .localRootTransform = {.translation = {5, 0, 0}, .scale = {2, 1, 1}},
                                                        .valueOverrides = layer}}};
    graph.sources.push_back({Take(PrefabDocument::Create({.projectVersion = Revision(93).projectVersion,
                                                          .assetId = Id(93),
                                                          .objects = {{.localId = {0}, .name = "Host"}},
                                                          .composition = std::move(composition),
                                                          .referencedAssets = {referenced}},
                                                         Limits())),
                             Revision(93)});
    const auto result = graph.Snapshot().Resolve(Id(93), PrefabInstanceId::Create(9).Value(), Limits());
    if (rejects) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == PrefabErrors::InvalidPlacement.code.Value());
        CHECK(original.Objects()[0].object.localTransform.rotation != Math::Quaternion{});
        return;
    }
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().Objects().size() == 2);
    const auto &nested = result.Value().Objects()[1];
    CHECK(nested.object.localTransform.rotation == Math::Quaternion{});
    CHECK(nested.effectiveLocalTransform.translation.x == 5.0F);
    CHECK((nested.effectiveLocalTransform.scale == Math::Vec3{2, 1, 1}));
    CHECK(nested.placementTransform.translation.x == 5.0F);
}

TEST_CASE("Malformed typed value diagnostics follow semantic identity rather than arrival order", "[prefab][override][diagnostics]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    auto set = graph.Assign(Asset(), Property(), 3.0);
    set.records[0].value = std::numeric_limits<double>::infinity();
    set.records.push_back({Object(), 1, {}, 3.0});
    const auto first = ValidatePrefabValueOverrideSet(set, Limits());
    std::ranges::reverse(set.records);
    const auto second = ValidatePrefabValueOverrideSet(set, Limits());
    REQUIRE(first.HasError());
    REQUIRE(second.HasError());
    CHECK(first.ErrorValue().code.Value() == second.ErrorValue().code.Value());
    CHECK(first.ErrorValue().message == second.ErrorValue().message);
    CHECK(Speed(graph.Resolve(Asset()), Object()) == 2.0);
}

TEST_CASE("Malformed source layers are diagnosed in stable placement identity order before payload expansion",
          "[prefab][override][source-order]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    graph.AddNested(92, 91, Revision(1));
    auto data = graph.sources.back().document.Data();
    data.composition->nestedPlacements[0].valueOverrides = graph.Assign(Asset(), Property(), 3.0);
    auto second = data.composition->nestedPlacements[0];
    second.placementLocalId = {8};
    data.composition->nestedPlacements.push_back(second);
    const auto document = Take(PrefabDocument::Create(std::move(data), Limits()));
    auto wire = nlohmann::ordered_json::parse(Take(document.SerializeCanonical()));
    wire["composition"]["nestedPlacements"][0]["valueOverrides"]["schema"] = 2;
    wire["composition"]["nestedPlacements"][1]["valueOverrides"]["sourcePrefab"] = "invalid";
    const auto first = PrefabDocument::Parse(wire.dump(2) + "\n", Limits());
    std::ranges::reverse(wire["composition"]["nestedPlacements"]);
    const auto reordered = PrefabDocument::Parse(wire.dump(2) + "\n", Limits());
    REQUIRE(first.HasError());
    REQUIRE(reordered.HasError());
    CHECK(first.ErrorValue().code.Value() == reordered.ErrorValue().code.Value());
    CHECK(first.ErrorValue().message == reordered.ErrorValue().message);
}

TEST_CASE("Two different malformed serialized rows preserve the complete public codec error after permutation",
          "[prefab][override][codec][diagnostics]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    const auto source = graph.Resolve(Asset());
    auto assigned = graph.Assign(Asset(), Property(), 3.0);
    const auto combined = Take(PreparePrefabValueEdit(source, assigned, {PrefabValueEditKind::Assign, Object(), Math::Transform{{1, 2, 3}}},
                                                      graph.schemas.get(), Limits()));
    auto wire = nlohmann::json::parse(Take(SerializePrefabValueOverrides(combined.rebased, Limits())));
    REQUIRE(wire["records"].size() == 2);
    wire["records"][0]["unexpected"] = true;
    wire["records"][1]["operation"] = 99;
    auto one = wire;
    one["records"] = nlohmann::json::array({wire["records"][0]});
    const auto firstKind = ParsePrefabValueOverrides(one.dump(0), Limits());
    one["records"] = nlohmann::json::array({wire["records"][1]});
    const auto secondKind = ParsePrefabValueOverrides(one.dump(0), Limits());
    REQUIRE(firstKind.HasError());
    REQUIRE(secondKind.HasError());
    REQUIRE(firstKind.ErrorValue().message != secondKind.ErrorValue().message);
    const auto first = ParsePrefabValueOverrides(wire.dump(0), Limits());
    std::ranges::reverse(wire["records"]);
    const auto reordered = ParsePrefabValueOverrides(wire.dump(0), Limits());
    REQUIRE(first.HasError());
    REQUIRE(reordered.HasError());
    CheckSameError(first.ErrorValue(), reordered.ErrorValue());
    CHECK_FALSE(first.ErrorValue().domain.Value().empty());
    CHECK_FALSE(first.ErrorValue().message.empty());
    CHECK(Speed(graph.Resolve(Asset()), Object()) == 2.0);
}

TEST_CASE("A later variant transform cannot silently approximate the shear introduced by an existing nested mount",
          "[prefab][override][mount]") {
    using namespace Horo::Prefab;
    using namespace LayerTest;
    Graph graph;
    graph.AddNested(92, 91, Revision(1));
    const auto original = graph.Resolve(Id(92));
    const Math::Transform rotation{.rotation = Math::Quaternion::FromAxisAngle({0, 0, 1}, 0.6F)};
    const auto intent = Take(PreparePrefabValueEdit(original, {}, {PrefabValueEditKind::Assign, NestedProperty().Object(), rotation},
                                                    graph.schemas.get(), Limits()));
    auto data = graph.sources.back().document.Data();
    data.composition->nestedPlacements[0].localRootTransform.scale = {2, 1, 1};
    graph.sources.back().document = Take(PrefabDocument::Create(std::move(data), Limits()));
    graph.AddVariant(93, 92, Revision(92), intent.rebased);
    const auto rejected = graph.Snapshot().Resolve(Id(93), PrefabInstanceId::Create(9).Value(), Limits());
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::InvalidPlacement.code.Value());
    CHECK(original.Objects()[1].object.localTransform.rotation == Math::Quaternion{});
}
