#include "Horo/Prefab/PrefabDebugSnapshot.h"
#include "Horo/Prefab/PrefabValueSnapshot.h"
#include "PrefabLayerOverrideTestSupport.h"
#include "PrefabValueOverrideTestSupport.h"

#include <algorithm>
#include <type_traits>

namespace Horo::Prefab {
    namespace {
        using namespace ValueTestSupport;

        /** @brief Reorders source documents and their variant operations while publishing moved asset paths. */
        PrefabSourceResolverSnapshot ReorderedVariantSnapshot(LayerTest::Graph &graph) {
            const auto prefabType = Assets::AssetTypeId::Parse("core.prefab").Value();
            auto sources = graph.sources;
            std::ranges::reverse(sources);
            std::vector<Assets::AssetRecord> records;
            for (auto &source : sources) {
                auto data = source.document.Data();
                const auto asset = data.assetId;
                if (data.composition && data.composition->variantOverrides) {
                    std::ranges::reverse(data.composition->variantOverrides->records);
                    source.document = LayerTest::Take(PrefabDocument::Create(std::move(data), Limits()));
                }
                const std::string path = "assets/moved/" + std::to_string(records.size()) + ".prefab";
                records.push_back({asset, prefabType, ProjectPath::Parse(path).Value(), ProjectPath::Parse(path + ".horo").Value()});
            }
            REQUIRE(graph.registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
            return LayerTest::Take(
                BuildPrefabSourceResolverSnapshot(graph.registry.Snapshot(), std::move(sources), Limits(), {}, graph.schemas));
        }

        TEST_CASE("Prefab variant cascade keeps applied layers and registered defaults inspectable after path changes",
                  "[unit][prefab][variant][debug]") {
            LayerTest::Graph graph;
            const auto base = Asset();
            const auto parent = LayerTest::Id(92);
            const auto derived = LayerTest::Id(93);
            const auto leaf = LayerTest::Id(94);
            const auto movedRoot = Math::Transform{{1, 2, 3}};
            const auto parentLayer =
                LayerTest::Take(PreparePrefabValueEdit(graph.Resolve(base), graph.Assign(base, Property(), 3.0),
                                                       {PrefabValueEditKind::Assign, Object(), movedRoot}, graph.schemas.get(), Limits()));
            graph.AddVariant(92, 91, LayerTest::Revision(1), parentLayer.rebased);
            graph.AddVariant(93, 92, LayerTest::Revision(92), graph.Assign(parent, Property(), 5.0));
            graph.AddParent(94, 93, LayerTest::Revision(93));
            const auto instance = PrefabInstanceId::Create(9).Value();
            const auto snapshot = graph.Snapshot();
            const auto original = LayerTest::Take(snapshot.ResolveOverrides(leaf, instance, Limits()));
            REQUIRE(original.effective);
            CHECK(original.effective->RootAsset() == leaf);
            CHECK(LayerTest::Speed(*original.effective, Object()) == 5.0);
            REQUIRE(original.layers.size() == 2);
            REQUIRE(original.layers[0].rebased.records.size() == 2);
            REQUIRE(original.layers[1].rebased.records.size() == 1);
            CHECK(original.layers[0].owner == parent);
            CHECK(original.layers[0].rebased.sourcePrefab == base);
            CHECK(original.effective->Objects()[0].object.localTransform == movedRoot);
            CHECK(original.layers[1].owner == derived);
            CHECK(original.layers[1].rebased.sourcePrefab == parent);
            CHECK(original.layers[1].rebased.records[0].value == PrefabOverrideValue{5.0});

            const auto moved = ReorderedVariantSnapshot(graph);
            const auto reordered = LayerTest::Take(moved.ResolveOverrides(leaf, instance, Limits()));
            REQUIRE(reordered.effective);
            CHECK(reordered.revision.registry != original.revision.registry);
            CHECK(moved.ValidateRevisionPublication(leaf, original.revision, {}, Limits()).HasValue());
            CHECK(std::ranges::equal(reordered.effective->Objects(), original.effective->Objects()));
            REQUIRE(reordered.layers.size() == original.layers.size());
            for (std::size_t index = 0; index < original.layers.size(); ++index) {
                CHECK(reordered.layers[index].owner == original.layers[index].owner);
                CHECK(reordered.layers[index].occurrence == original.layers[index].occurrence);
                CHECK(reordered.layers[index].rebased == original.layers[index].rebased);
            }
        }

        TEST_CASE("Prefab variant value inspection separates the schema default from inherited and instance values",
                  "[unit][prefab][variant][debug]") {
            LayerTest::Graph graph;
            const auto parent = LayerTest::Id(92);
            const auto derived = LayerTest::Id(93);
            graph.AddVariant(92, 91, LayerTest::Revision(1), graph.Assign(Asset(), Property(), 3.0));
            graph.AddVariant(93, 92, LayerTest::Revision(92), graph.Assign(parent, Property(), 5.0));
            const auto inherited = graph.Resolve(derived);
            const auto instance = graph.Assign(derived, Property(), 7.0);
            const auto inspected = LayerTest::Take(PrefabValueSnapshot::Prepare(inherited, instance, graph.schemas.get(), Limits()));
            REQUIRE(inspected.snapshot);
            REQUIRE(inspected.resolution.effective);
            const auto values = inspected.snapshot->Values();
            const auto property = std::ranges::find(values, PrefabValueOverrideTarget{Property()}, &PrefabValueProvenance::target);
            REQUIRE(property != values.end());
            CHECK(property->schemaDefault == PrefabOverrideValue{1.0});
            CHECK(property->sourceValue == PrefabOverrideValue{5.0});
            CHECK(property->effectiveValue == PrefabOverrideValue{7.0});
            CHECK(property->origin == PrefabValueOrigin::Instance);
            CHECK(property->sourcePrefab == Asset());
            const std::vector expected{PrefabCompositionOriginStep{PrefabCompositionOrigin::Variant, derived, parent},
                                       PrefabCompositionOriginStep{PrefabCompositionOrigin::Variant, parent, Asset()}};
            CHECK(property->originChain == expected);
            CHECK(LayerTest::Speed(inherited, Object()) == 5.0);
        }

        TEST_CASE("Prefab debug snapshot retains exact source graph and configured budgets", "[unit][prefab][debug]") {
            const auto source = Source();
            const auto result = CapturePrefabDebugSnapshot(source, Limits());
            REQUIRE(result.HasValue());
            const auto &snapshot = result.Value();
            CHECK(snapshot.RootAsset() == Asset());
            CHECK(snapshot.SourceRevision() == source.Revision().rootSource);
            CHECK(snapshot.RegistryRevision() == source.Revision().registry);
            CHECK(snapshot.Policy() == Limits().Policy());
            CHECK(snapshot.ExpansionWorkCeiling() == Limits().MaximumExpansionWorkItems());
            CHECK(snapshot.ParserByteCeiling() == PrefabHardLimits::SourceDocumentBytes);
            CHECK(snapshot.ParserDepthCeiling() == PrefabHardLimits::SourceJsonDepth);
            REQUIRE(snapshot.Nodes().size() == 1);
            CHECK(snapshot.Nodes()[0].assetId == Asset());
            REQUIRE(snapshot.Objects().size() == 1);
            CHECK(snapshot.Objects()[0].key == source.Objects()[0].key);
            CHECK(snapshot.Objects()[0].componentCount == 1);
            CHECK_FALSE(snapshot.Overrides().supplied);
            CHECK_FALSE(snapshot.IsTruncated());
        }

        TEST_CASE("Prefab debug snapshot has exact zero and boundary prefix truncation", "[unit][prefab][debug][boundary]") {
            const auto source = Source();
            const auto totals = CapturePrefabDebugSnapshot(source, Limits(), {0}).Value();
            CHECK(totals.IsTruncated());
            CHECK(totals.NodeCoverage().available == 1);
            CHECK(totals.NodeCoverage().Omitted() == 1);
            CHECK(totals.ObjectCoverage().Omitted() == 1);
            CHECK(totals.Nodes().empty());
            CHECK(totals.Objects().empty());
            const auto partial = CapturePrefabDebugSnapshot(source, Limits(), {1}).Value();
            CHECK(partial.Nodes().size() == 1);
            CHECK(partial.Objects().empty());
            CHECK(partial.ObjectCoverage().Omitted() == 1);
            CHECK_FALSE(CapturePrefabDebugSnapshot(source, Limits(), {2}).Value().IsTruncated());
            const auto excessive = CapturePrefabDebugSnapshot(source, Limits(), {PrefabDebugCaptureLimits::MaximumEntries + 1});
            REQUIRE(excessive.HasError());
            CHECK(excessive.ErrorValue().code.Value() == PrefabErrors::DiagnosticBudgetExceeded.code.Value());
        }

        TEST_CASE("Prefab debug snapshot preserves semantic graph edges and repeated nested identity scopes",
                  "[unit][prefab][debug][identity]") {
            const auto base = Sources().Sources()[0];
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = 92;
            const auto outer = Assets::AssetId::FromBytes(bytes);
            Assets::AssetRegistry registry;
            REQUIRE(
                registry
                    .Publish({{Asset(), Assets::AssetTypeId::Parse("core.prefab").Value(), ProjectPath::Parse("assets/base.prefab").Value(),
                               ProjectPath::Parse("assets/base.prefab.horo").Value()},
                              {outer, Assets::AssetTypeId::Parse("core.prefab").Value(), ProjectPath::Parse("assets/outer.prefab").Value(),
                               ProjectPath::Parse("assets/outer.prefab.horo").Value()}})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
            PrefabComposition composition;
            for (const auto slot : {3U, 4U})
                composition.nestedPlacements.push_back({{slot}, {}, PrefabAssetReference::Create(Asset()).Value(), base.sourceRevision});
            const auto document = PrefabDocument::Create({.projectVersion = base.sourceRevision.projectVersion,
                                                          .assetId = outer,
                                                          .objects = {{.localId = {0}, .name = "Outer"}},
                                                          .composition = composition,
                                                          .referencedAssets = {Asset()}},
                                                         Limits())
                                      .Value();
            auto revision = base.sourceRevision;
            revision.contentDigest.bytes.front() = 1;
            const auto sources = BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {base, {document, revision}}, Limits()).Value();
            const auto source = sources.Resolve(outer, PrefabInstanceId::Create(9).Value(), Limits()).Value();
            const auto snapshot = CapturePrefabDebugSnapshot(source, Limits()).Value();
            REQUIRE(snapshot.Nodes().size() == 2);
            REQUIRE(snapshot.Edges().size() == 1);
            CHECK(snapshot.Edges()[0].sourcePrefab == outer);
            CHECK(snapshot.Edges()[0].targetAsset == Asset());
            CHECK(snapshot.Edges()[0].kind == PrefabDependencyKind::NestedPrefab);
            REQUIRE(snapshot.Objects().size() == 3);
            CHECK(snapshot.Objects()[1].sourcePrefab == Asset());
            CHECK(snapshot.Objects()[2].sourcePrefab == Asset());
            REQUIRE(snapshot.Objects()[1].key.object.NestedInstanceScope().size() == 1);
            REQUIRE(snapshot.Objects()[2].key.object.NestedInstanceScope().size() == 1);
            CHECK(snapshot.Objects()[1].key.object.NestedInstanceScope()[0] == LocalObjectId{3});
            CHECK(snapshot.Objects()[2].key.object.NestedInstanceScope()[0] == LocalObjectId{4});
            CHECK(snapshot.Objects()[1].key != snapshot.Objects()[2].key);
            CHECK(snapshot.Objects()[1].parent == snapshot.Objects()[0].key);
            const auto partial = CapturePrefabDebugSnapshot(source, Limits(), {4}).Value();
            CHECK(partial.Objects().size() == 1);
            CHECK(partial.ObjectCoverage().Omitted() == 2);
        }

        TEST_CASE("Prefab debug snapshot owns effective override targets and provenance after retirement",
                  "[unit][prefab][debug][lifecycle]") {
            const auto snapshot = [] {
                const auto source = Source();
                const auto schemas = Schemas();
                const auto resolution =
                    PreparePrefabValueEdit(source, std::nullopt, {PrefabValueEditKind::Assign, Property(), 3.0}, &schemas, Limits())
                        .Value();
                REQUIRE(resolution.effective);
                return CapturePrefabDebugSnapshot(*resolution.effective, Limits(), {}, &resolution.rebased, resolution.provenance,
                                                  resolution.redundantRecords)
                    .Value();
            }();
            CHECK(snapshot.Overrides().supplied);
            CHECK(snapshot.Overrides().assignments == 1);
            REQUIRE(snapshot.OverrideRecords().size() == 1);
            CHECK(snapshot.OverrideRecords()[0].state == PrefabDebugOverrideState::Active);
            CHECK(snapshot.OverrideRecords()[0].kind == PrefabDebugOverrideKind::AssignValue);
            CHECK(std::get<PrefabValueOverrideTarget>(snapshot.OverrideRecords()[0].target) == PrefabValueOverrideTarget{Property()});
            REQUIRE_FALSE(snapshot.Values().empty());
            CHECK(snapshot.Values().back().origin == PrefabValueOrigin::Instance);
            CHECK(snapshot.Values().back().sourcePrefab == Asset());
            CHECK(snapshot.ValueOverridesDigest() != Sha256Digest{});
        }

        TEST_CASE("Prefab debug snapshot preserves unresolved value and topology intent without payload copying", "[unit][prefab][debug]") {
            const auto source = Source();
            const auto schemas = Schemas();
            auto assigned =
                PreparePrefabValueEdit(source, std::nullopt, {PrefabValueEditKind::Assign, Property(), 3.0}, &schemas, Limits()).Value();
            const auto changed = Source("{\"speed\":4.0}", 2);
            const auto conflict = ResolvePrefabValueOverrides(changed, assigned.rebased, &schemas, Limits()).Value();
            REQUIRE_FALSE(conflict.effective);
            auto snapshot = CapturePrefabDebugSnapshot(changed, Limits(), {}, &conflict.rebased, conflict.provenance).Value();
            CHECK(snapshot.Overrides().conflicts == 1);
            REQUIRE(snapshot.OverrideRecords().size() == 1);
            CHECK(snapshot.OverrideRecords()[0].state == PrefabDebugOverrideState::Conflict);
            const auto orphan = ResolvePrefabValueOverrides(source, assigned.rebased, nullptr, Limits()).Value();
            snapshot = CapturePrefabDebugSnapshot(source, Limits(), {}, &orphan.rebased, orphan.provenance).Value();
            CHECK(snapshot.Overrides().orphans == 1);
            CHECK(snapshot.OverrideRecords()[0].state == PrefabDebugOverrideState::Orphan);
            auto topology = orphan.rebased;
            topology.componentRecords.emplace_back(
                PrefabRemoveComponentOverride{{Object(), Type(), PrefabComponentInstanceId::Create(1).Value()}, 1, {}});
            snapshot = CapturePrefabDebugSnapshot(source, Limits(), {}, &topology).Value();
            CHECK(snapshot.Overrides().componentOperations == 1);
            CHECK(snapshot.OverrideRecords().back().kind == PrefabDebugOverrideKind::RemoveComponent);
        }

        TEST_CASE("Prefab debug capture rejects foreign and malformed retained provenance and honours cancellation",
                  "[unit][prefab][debug][malformed][lifecycle]") {
            const auto source = Source();
            const auto schemas = Schemas();
            auto resolution =
                PreparePrefabValueEdit(source, std::nullopt, {PrefabValueEditKind::Assign, Property(), 3.0}, &schemas, Limits()).Value();
            auto foreign = resolution.rebased;
            foreign.sourcePrefab = {};
            REQUIRE(CapturePrefabDebugSnapshot(source, Limits(), {}, &foreign).HasError());
            auto malformed = resolution.provenance;
            malformed[0].origin = static_cast<PrefabValueOrigin>(255);
            REQUIRE(CapturePrefabDebugSnapshot(source, Limits(), {}, &resolution.rebased, malformed).HasError());
            // Omitted provenance is explicitly uninspected; zero budget still reports its exact omitted count.
            CHECK(CapturePrefabDebugSnapshot(source, Limits(), {0}, &resolution.rebased, malformed).Value().ValueCoverage().Omitted() ==
                  malformed.size());
            malformed = resolution.provenance;
            malformed[0].sourcePrefab = {};
            REQUIRE(CapturePrefabDebugSnapshot(source, Limits(), {}, &resolution.rebased, malformed).HasError());
            malformed = resolution.provenance;
            malformed[0].target = PrefabObjectAddress::Create({}, {99}).Value();
            REQUIRE(CapturePrefabDebugSnapshot(source, Limits(), {}, &resolution.rebased, malformed).HasError());
            CancellationSource cancelled;
            cancelled.RequestCancellation();
            const auto result = CapturePrefabDebugSnapshot(source, Limits(), {}, nullptr, {}, 0, cancelled.Token());
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == PrefabErrors::Cancelled.code.Value());
            CHECK(source.Objects()[0].object.name == "Root");
            CHECK(source.ValueOverridesDigest() == Sha256Digest{});
        }

        // Constructor-bounded dynamic identity text is included; no row retains arbitrary payload/name/value strings.
        static_assert(sizeof(PrefabDependencyNode) + 96 <= 512);
        static_assert(sizeof(PrefabDependencyEdge) <= 512);
        static_assert(sizeof(PrefabDebugObject) <= 512);
        static_assert(sizeof(PrefabDebugValue) + Gameplay::MaximumComponentTypeIdBytes <= 512);
        static_assert(sizeof(PrefabDebugOverride) + Gameplay::MaximumComponentTypeIdBytes <= 512);
        static_assert(PrefabDebugCaptureLimits::MaximumEntries * 512 == 2U * 1024U * 1024U);
    }  // namespace
}  // namespace Horo::Prefab
