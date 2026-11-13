#include "../../support/AllocationProbe.h"
#include "../../support/SceneAllocationQualification.h"
#include "../runtime/prefab/PrefabLayerOverrideTestSupport.h"
#include "PrefabDocumentSessionTestSupport.h"

#include <fstream>

namespace Horo::Editor {
    namespace {
        using namespace Prefab::LayerTest;
        using namespace Prefab::ValueTestSupport;
        using Fixture = PrefabDocumentSessionTestSupport::Fixture;

        /** @brief Opens an actual canonical layered source through the same document/sidecar authority used by the workspace. */
        void OpenLayer(Fixture &fixture, bool nested = false) {
            const auto previous = fixture.document->Snapshot();
            REQUIRE(fixture.document->Close(previous.fence, true).HasValue());
            fixture.document.reset();
            Prefab::PrefabDocumentData data{.projectVersion = Revision(1).projectVersion, .assetId = fixture.project.asset};
            if (nested) {
                data.objects = {{.localId = {0}, .name = "Host"}};
                data.composition =
                    Prefab::PrefabComposition{.nestedPlacements = {{.placementLocalId = {7},
                                                                    .sourcePrefab = Take(Prefab::PrefabAssetReference::Create(Asset())),
                                                                    .authoredAgainst = Revision(1)}}};
            } else
                data.composition = Prefab::PrefabComposition{.variantParent = Take(Prefab::PrefabAssetReference::Create(Asset())),
                                                             .variantAuthoredAgainst = Revision(1)};
            data.referencedAssets = {Asset()};
            const auto source = Take(Prefab::PrefabDocument::Create(std::move(data), fixture.limits));
            {
                std::ofstream output(fixture.project.source, std::ios::binary | std::ios::trunc);
                output << Take(source.SerializeCanonical());
            }
            fixture.document =
                Take(PrefabDocumentSession::Open(fixture.project.directory.path, fixture.project.source, fixture.project.asset,
                                                 previous.identity, previous.fence.owner, fixture.limits));
        }

        /** @brief Sends typed author intent rather than replacing serialized layer bytes from a test helper. */
        Result<void> Edit(Fixture &fixture, const Prefab::PrefabSourceResolverSnapshot &sources, Prefab::PrefabValueEditKind kind,
                          double value = 3.0, std::optional<Prefab::LocalObjectId> placement = {}) {
            return fixture.document->ExecuteLayer(fixture.document->Snapshot().fence, placement, sources,
                                                  Prefab::PrefabValueEdit{kind, Property(), value});
        }
    }  // namespace

    TEST_CASE("Prefab source layer transactions apply revert undo redo and stale fences without changing referenced source",
              "[editor][prefab][layer]") {
        Fixture fixture;
        OpenLayer(fixture);
        Graph graph;
        const auto sources = graph.Snapshot();
        const auto original = Take(fixture.document->Snapshot().source->SerializeCanonical());
        REQUIRE(Edit(fixture, sources, Prefab::PrefabValueEditKind::Assign).HasValue());
        const auto accepted = fixture.document->Snapshot();
        REQUIRE(accepted.source->Data().composition->variantOverrides);
        CHECK(accepted.source->Data().composition->variantOverrides->records.size() == 1);
        REQUIRE(fixture.document->Replay(accepted.fence, false).HasValue());
        CHECK(Take(fixture.document->Snapshot().source->SerializeCanonical()) == original);
        REQUIRE(fixture.document->Replay(fixture.document->Snapshot().fence, true).HasValue());
        CHECK(Take(fixture.document->Snapshot().source->SerializeCanonical()) == Take(accepted.source->SerializeCanonical()));
        CHECK(fixture.document
                  ->ExecuteLayer(accepted.fence, {}, sources, Prefab::PrefabValueEdit{Prefab::PrefabValueEditKind::Revert, Property(), 0.0})
                  .HasError());
        REQUIRE(Edit(fixture, sources, Prefab::PrefabValueEditKind::Revert).HasValue());
        CHECK_FALSE(fixture.document->Snapshot().source->Data().composition->variantOverrides);
        CHECK(Speed(graph.Resolve(Asset()), Object()) == 2.0);
    }

    TEST_CASE("Rebasing a variant source preserves exact conflict evidence in source history until explicit assignment",
              "[editor][prefab][revision]") {
        Fixture fixture;
        OpenLayer(fixture);
        Graph graph;
        REQUIRE(Edit(fixture, graph.Snapshot(), Prefab::PrefabValueEditKind::Assign).HasValue());
        const auto accepted = fixture.document->Snapshot();
        graph.sources[0] = Sources("{\"speed\":4.0}", 2).Sources()[0];
        const auto current = graph.Snapshot();
        REQUIRE(Edit(fixture, current, Prefab::PrefabValueEditKind::Rebase).HasValue());
        const auto rebased = fixture.document->Snapshot();
        REQUIRE(rebased.source->Data().composition->variantOverrides);
        const auto &layer = *rebased.source->Data().composition->variantOverrides;
        REQUIRE(layer.conflicts.size() == 1);
        CHECK(layer.conflicts[0].authoredAgainst == Revision(1));
        CHECK(layer.conflicts[0].observedSource == Revision(2));
        CHECK(*rebased.source->Data().composition->variantAuthoredAgainst == Revision(2));
        REQUIRE(fixture.document->Replay(rebased.fence, false).HasValue());
        CHECK(Take(fixture.document->Snapshot().source->SerializeCanonical()) == Take(accepted.source->SerializeCanonical()));
        REQUIRE(Edit(fixture, current, Prefab::PrefabValueEditKind::Assign, 5.0).HasValue());
        CHECK_FALSE(fixture.document->Snapshot().source->Data().composition->variantOverrides->HasUnresolved());
    }

    TEST_CASE("Nested source layer cancellation and malformed selectors preserve document source and history",
              "[editor][prefab][lifecycle]") {
        Fixture fixture;
        OpenLayer(fixture, true);
        Graph graph;
        const auto sources = graph.Snapshot();
        REQUIRE(Edit(fixture, sources, Prefab::PrefabValueEditKind::Assign, 3.0, Prefab::LocalObjectId{7}).HasValue());
        const auto accepted = fixture.document->Snapshot();
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        CHECK(fixture.document
                  ->ExecuteLayer(accepted.fence, Prefab::LocalObjectId{7}, sources,
                                 Prefab::PrefabValueEdit{Prefab::PrefabValueEditKind::Revert, Property(), 0.0}, cancellation.Token())
                  .HasError());
        CHECK(fixture.document
                  ->ExecuteLayer(accepted.fence, Prefab::LocalObjectId{8}, sources,
                                 Prefab::PrefabValueEdit{Prefab::PrefabValueEditKind::Revert, Property(), 0.0})
                  .HasError());
        const auto current = fixture.document->Snapshot();
        CHECK(current.fence.revision == accepted.fence.revision);
        CHECK(current.canUndo == accepted.canUndo);
        CHECK(Take(current.source->SerializeCanonical()) == Take(accepted.source->SerializeCanonical()));
    }
}  // namespace Horo::Editor

TEST_CASE("Source layer preparation preserves source revision and history at each injected allocation failure",
          "[editor][prefab][allocation]") {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Prefab::LayerTest;
    using namespace Prefab::ValueTestSupport;
    Tests::RequireSupportedSceneAllocationInjection();
    PrefabDocumentSessionTestSupport::Fixture fixture;
    // The shared real-file setup helpers remain in Editor's translation-unit namespace.
    OpenLayer(fixture);
    Graph graph;
    const auto sources = graph.Snapshot();
    const auto before = fixture.document->Snapshot();
    const std::variant<Prefab::PrefabValueEdit, Prefab::PrefabComponentEdit> intent{
        Prefab::PrefabValueEdit{Prefab::PrefabValueEditKind::Assign, Property(), 3.0}};
    // Standard-library allocation counts differ by platform. Measure the real successful cold operation before injecting every site.
    const auto allocations = [&] {
        PrefabDocumentSessionTestSupport::Fixture probe;
        OpenLayer(probe);
        std::size_t requests{};
        const auto result = [&] {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            auto edited = probe.document->ExecuteLayer(probe.document->Snapshot().fence, {}, sources, intent);
            requests = measurement.Snapshot().requests;
            return edited;
        }();
        REQUIRE(result.HasValue());
        return requests;
    }();
    REQUIRE(allocations < 32768);
    bool completed = false;
    for (std::size_t failure = 0; failure <= allocations * 2; ++failure) {
        INFO("Actual source-layer allocation failure index: " << failure);
        auto result = [&] {
            Tests::AllocationProbe::ScopedFailure allocation(failure);
            return fixture.document->ExecuteLayer(before.fence, {}, sources, intent);
        }();
        if (result.HasValue()) {
            completed = true;
            break;
        }
        const auto after = fixture.document->Snapshot();
        CHECK(after.fence.revision == before.fence.revision);
        CHECK(after.canUndo == before.canUndo);
        CHECK(Take(after.source->SerializeCanonical()) == Take(before.source->SerializeCanonical()));
    }
    REQUIRE(completed);
}
