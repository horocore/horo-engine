#include "Horo/PCG/PCGErrors.h"
#include "PCGDocumentTestSupport.h"

#include <limits>

namespace Horo::Editor {
    namespace {
        using namespace PCGDocumentTestSupport;

        PCG::PCGNodeCatalog Owner() {
            const std::array grants{PCG::PCGCapability::Validation, PCG::PCGCapability::OfflineBake};
            auto owner = PCG::PCGNodeCatalog::Create(
                PCG::ProjectPCGCapabilities(PCG::PCGHostProfile::Headless, PCG::PCGCapabilitySet::Create(grants).Value()).Value());
            REQUIRE(owner.HasValue());
            return std::move(owner).Value();
        }

        struct Fixture final {
            PCG::PCGNodeCatalog owner{Owner()};
            PCG::PCGNodeCatalogSnapshot catalog;
            PCGGraphDocument document;

            explicit Fixture(PCG::PCGCpuNodeKind kind = PCG::PCGCpuNodeKind::GridJitter) : document(Open(kind)) {}

            PCGGraphDocument Open(PCG::PCGCpuNodeKind kind) {
                REQUIRE(owner.Register(kind).HasValue());
                catalog = owner.Snapshot().Value();
                const auto &descriptor = catalog.Nodes().front();
                auto data = Graph();
                data.nodes.clear();
                data.edges.clear();
                data.exposedInputs.clear();
                PCG::PCGGraphNode node{.id = Id<PCG::NodeId>(1), .type = descriptor.type, .version = descriptor.version};
                for (std::size_t i = 0; i < descriptor.pinCount; ++i) {
                    const auto &pin = descriptor.pins[i];
                    node.pins.push_back({Id<PCG::PinId>(i + 1), pin.direction, pin.type, pin.cardinality, {}});
                }
                if (kind == PCG::PCGCpuNodeKind::Stratified)
                    node.payload =
                        PCG::EncodePCGPointGenerationSettings(PCG::PCGStratifiedGenerationSettings{Id<PCG::SpatialElementId>(1)}).Value();
                else if (kind == PCG::PCGCpuNodeKind::PoissonScatter)
                    node.payload =
                        PCG::EncodePCGPointGenerationSettings(PCG::PCGPoissonGenerationSettings{.volume = Id<PCG::SpatialElementId>(1)})
                            .Value();
                else
                    node.payload =
                        PCG::EncodePCGPointGenerationSettings(PCG::PCGJitterGenerationSettings{Id<PCG::SpatialElementId>(1), 0.5}).Value();
                data.nodes.push_back(node);
                auto support = Catalog();
                support.types = {{descriptor.type, descriptor.version, descriptor.version}};
                auto result = PCGGraphDocument::Open(Asset(), Identity(), {Asset(9), 1}, Capture(data), support);
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            PCGGraphAuthoringDraft Draft() {
                return PCGGraphAuthoringDraft::Capture(document.Snapshot().Value(), catalog).Value();
            }
        };
    }  // namespace

    TEST_CASE("PCG inspector seed and schema properties commit as one reversible transaction", "[editor][pcg][authoring]") {
        Fixture fixture;
        auto draft = fixture.Draft();
        const auto before = fixture.document.Snapshot().Value();
        draft.values.deterministicSeed = std::numeric_limits<std::uint64_t>::max();
        std::get<PCG::PCGJitterGenerationSettings>(*draft.properties[0].settings).fraction = 1.0;
        auto command = draft.Command(before, fixture.catalog, true);
        REQUIRE(command.HasValue());
        REQUIRE(command.Value().operations.size() == 2);
        REQUIRE(fixture.document.Execute(command.Value()).HasValue());
        CHECK(fixture.document.UndoCount() == 1);
        auto after = fixture.document.Snapshot().Value();
        CHECK(after.source->Data().deterministicSeed == std::numeric_limits<std::uint64_t>::max());
        CHECK(std::get<PCG::PCGJitterGenerationSettings>(fixture.catalog.DecodeSettings(after.source->Data().nodes[0]).Value()).fraction ==
              1.0);
        REQUIRE(fixture.document.Replay(Fence(after), false).HasValue());
        CHECK(fixture.document.Snapshot().Value().source->Data() == before.source->Data());
        REQUIRE(fixture.document.Replay(Fence(fixture.document.Snapshot().Value()), true).HasValue());
        CHECK(fixture.document.Snapshot().Value().source->Data() == after.source->Data());
    }

    TEST_CASE("PCG inspector failure and cancelled drafts preserve source history and unsupported modes", "[editor][pcg][authoring]") {
        Fixture fixture;
        const auto before = fixture.document.Snapshot().Value();
        auto draft = fixture.Draft();
        draft.values.mode = PCG::PCGGenerationMode::Runtime;
        ErrorIs(draft.Command(before, fixture.catalog, true), PCG::PCGErrors::UnsupportedCapability);
        CHECK(draft.values.mode == PCG::PCGGenerationMode::Runtime);
        draft.values.mode = PCG::PCGGenerationMode::Offline;
        auto &settings = std::get<PCG::PCGJitterGenerationSettings>(*draft.properties[0].settings);
        for (const double fraction : {-0.01, 1.01, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            settings.fraction = fraction;
            CHECK(draft.Command(before, fixture.catalog, true).HasError());
        }
        settings.fraction = 0;
        REQUIRE(draft.Command(before, fixture.catalog, true).HasValue());
        ErrorIs(draft.Command(before, fixture.catalog, false), PCGDocumentError(PCGDocumentCommandFailureReason::Unauthorized));
        CHECK(fixture.document.Snapshot().Value().state == before.state);
        CHECK(fixture.document.UndoCount() == 0);
    }

    TEST_CASE("PCG inspector replacement reload history and close reject stale gestures", "[editor][pcg][authoring]") {
        Fixture fixture;
        auto draft = fixture.Draft();
        draft.values.deterministicSeed = 101;
        REQUIRE(fixture.document.Execute(Edit(fixture.document.Snapshot().Value(), 42)).HasValue());
        CHECK_FALSE(draft.Matches(fixture.document.Snapshot().Value(), fixture.catalog));
        CHECK(draft.Command(fixture.document.Snapshot().Value(), fixture.catalog, true).HasError());
        draft = fixture.Draft();
        draft.values.deterministicSeed = 101;
        REQUIRE(fixture.owner.Register(PCG::PCGCpuNodeKind::GridJitter, true).HasValue());
        const auto replaced = fixture.owner.Snapshot().Value();
        CHECK_FALSE(draft.Matches(fixture.document.Snapshot().Value(), replaced));
        CHECK(draft.Command(fixture.document.Snapshot().Value(), replaced, true).HasError());
        auto command = draft.Command(fixture.document.Snapshot().Value(), fixture.catalog, true);
        REQUIRE(command.HasValue());
        REQUIRE(fixture.document.Close(true).HasValue());
        CHECK(fixture.document.Execute(command.Value()).HasError());
    }

    TEST_CASE("PCG inspector preserves unavailable payloads while editing known graph defaults", "[editor][pcg][authoring]") {
        Project project;
        auto document = project.Open();
        auto owner = Owner();
        const auto catalog = owner.Snapshot().Value();
        const auto before = document.Snapshot().Value();
        auto draft = PCGGraphAuthoringDraft::Capture(before, catalog).Value();
        REQUIRE_FALSE(draft.properties[0].settings.has_value());
        REQUIRE(draft.properties[0].diagnostic.has_value());
        draft.values.deterministicSeed = 0;
        draft.values.exposedInputs[0].defaultValue = 0.0;
        auto command = draft.Command(before, catalog, true);
        REQUIRE(command.HasValue());
        REQUIRE(command.Value().operations.size() == 2);
        REQUIRE(document.Execute(command.Value()).HasValue());
        CHECK(document.Snapshot().Value().source->Data().nodes == before.source->Data().nodes);
        CHECK(document.UndoCount() == 1);
    }

    TEST_CASE("PCG inspector rejects invalid input defaults and structural edits atomically", "[editor][pcg][authoring]") {
        Project project;
        auto document = project.Open();
        auto owner = Owner();
        const auto catalog = owner.Snapshot().Value();
        const auto before = document.Snapshot().Value();
        auto draft = PCGGraphAuthoringDraft::Capture(before, catalog).Value();
        draft.values.deterministicSeed = 0;
        draft.values.exposedInputs[0].defaultValue = std::uint64_t{3};
        CHECK(draft.Command(before, catalog, true).HasError());
        draft.values.exposedInputs[0].defaultValue = 0.0;
        draft.values.nodes[0].payload.clear();
        CHECK(draft.Command(before, catalog, true).HasError());
        CHECK(document.Snapshot().Value().state == before.state);
        CHECK(document.UndoCount() == 0);
    }

    TEST_CASE("PCG inspector generation values enforce complete operational tier boundaries", "[editor][pcg][authoring]") {
        const auto maximum = PCG::LimitsForTier(PCG::PCGOperationalTier::Baseline).Value().maximumPointsPerNodeOutput;
        Fixture poisson{PCG::PCGCpuNodeKind::PoissonScatter};
        auto draft = poisson.Draft();
        auto &scatter = std::get<PCG::PCGPoissonGenerationSettings>(*draft.properties[0].settings);
        scatter.count = static_cast<std::uint32_t>(maximum);
        scatter.attempts = scatter.count;
        REQUIRE(draft.Command(poisson.document.Snapshot().Value(), poisson.catalog, true).HasValue());
        ++scatter.count;
        ++scatter.attempts;
        ErrorIs(draft.Command(poisson.document.Snapshot().Value(), poisson.catalog, true),
                PCGDocumentError(PCGDocumentCommandFailureReason::Limit));
        Fixture strata{PCG::PCGCpuNodeKind::Stratified};
        draft = strata.Draft();
        auto &lattice = std::get<PCG::PCGStratifiedGenerationSettings>(*draft.properties[0].settings);
        lattice.strata = {static_cast<std::uint32_t>(maximum), 1, 1};
        REQUIRE(draft.Command(strata.document.Snapshot().Value(), strata.catalog, true).HasValue());
        lattice.strata = {std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max(), 2};
        ErrorIs(draft.Command(strata.document.Snapshot().Value(), strata.catalog, true),
                PCGDocumentError(PCGDocumentCommandFailureReason::Limit));
        CHECK(poisson.document.UndoCount() == 0);
        CHECK(strata.document.UndoCount() == 0);
    }

    TEST_CASE("PCG inspector reload and new-session replacement invalidate a pending apply", "[editor][pcg][authoring]") {
        Project project;
        auto document = project.Open();
        auto owner = Owner();
        const auto catalog = owner.Snapshot().Value();
        auto draft = PCGGraphAuthoringDraft::Capture(document.Snapshot().Value(), catalog).Value();
        draft.values.deterministicSeed = 123;
        REQUIRE(document.Reload(Fence(document.Snapshot().Value()), Capture(Graph(2)), false).HasValue());
        CHECK(draft.Command(document.Snapshot().Value(), catalog, true).HasError());
        auto replacement = PCGGraphDocument::Open(Asset(), Identity(), {Asset(9), 2}, Capture(), Catalog());
        REQUIRE(replacement.HasValue());
        CHECK(draft.Command(replacement.Value().Snapshot().Value(), catalog, true).HasError());
        CHECK(document.UndoCount() == 0);
        CHECK(replacement.Value().UndoCount() == 0);
    }

    TEST_CASE("PCG graph input creation removal and renaming use the same atomic document history", "[editor][pcg][authoring]") {
        Project project;
        auto document = project.Open();
        auto owner = Owner();
        const auto catalog = owner.Snapshot().Value();
        const auto before = document.Snapshot().Value();
        auto draft = PCGGraphAuthoringDraft::Capture(before, catalog).Value();
        auto input = draft.values.exposedInputs.front();
        draft.values.exposedInputs.clear();
        input.id = Id<PCG::ExposedInputId>(2);
        input.key = "world.new_density";
        input.defaultValue = 0.25;
        draft.values.exposedInputs.push_back(input);
        draft.values.deterministicSeed = 0;
        const auto command = draft.Command(before, catalog, true);
        REQUIRE(command.HasValue());
        CHECK(command.Value().operations.size() == 3);
        REQUIRE(document.Execute(command.Value()).HasValue());
        CHECK(document.UndoCount() == 1);
        const auto after = document.Snapshot().Value();
        REQUIRE(after.source->Data().exposedInputs.size() == 1);
        CHECK(after.source->Data().exposedInputs[0] == input);
        REQUIRE(document.Replay(Fence(after), false).HasValue());
        CHECK(document.Snapshot().Value().source->Data() == before.source->Data());
        draft = PCGGraphAuthoringDraft::Capture(document.Snapshot().Value(), catalog).Value();
        draft.values.exposedInputs[0].key = "world.renamed_density";
        REQUIRE(document.Execute(draft.Command(document.Snapshot().Value(), catalog, true).Value()).HasValue());
        CHECK(document.Snapshot().Value().source->Data().exposedInputs[0].key == "world.renamed_density");
        draft = PCGGraphAuthoringDraft::Capture(document.Snapshot().Value(), catalog).Value();
        input = draft.values.exposedInputs[0];
        input.id = Id<PCG::ExposedInputId>(2);
        input.node = Id<PCG::NodeId>(2);
        input.pin = Id<PCG::PinId>(3);
        draft.values.exposedInputs.push_back(input);
        CHECK(draft.Command(document.Snapshot().Value(), catalog, true).HasError());
    }

    TEST_CASE("PCG graph mode changes require both a granted capability and a compatible source tier", "[editor][pcg][authoring]") {
        const std::array grants{PCG::PCGCapability::Validation, PCG::PCGCapability::OfflineBake, PCG::PCGCapability::RuntimeEvaluation,
                                PCG::PCGCapability::HybridEvaluation};
        auto owner = PCG::PCGNodeCatalog::Create({PCG::PCGHostProfile::Interactive, PCG::PCGCapabilitySet::Create(grants).Value()}).Value();
        const auto catalog = owner.Snapshot().Value();
        auto source = Graph();
        source.tier = PCG::PCGOperationalTier::Standard;
        auto support = Catalog();
        support.tier = source.tier;
        auto graph = PCG::PCGGraphAsset::Create(source, {.tier = source.tier, .supportedNodeTypes = support.types});
        REQUIRE(graph.HasValue());
        const auto encoded = PCG::SerializePCGGraphAsset(graph.Value()).Value();
        const auto bytes = std::as_bytes(std::span(encoded));
        const Assets::AssetSourceCapture capture{{true, bytes.size(), ComputeSha256(bytes)}, {bytes.begin(), bytes.end()}};
        auto opened = PCGGraphDocument::Open(Asset(), Identity(), {Asset(9), 1}, capture, support);
        REQUIRE(opened.HasValue());
        auto document = std::move(opened).Value();
        for (const auto mode : {PCG::PCGGenerationMode::Runtime, PCG::PCGGenerationMode::Hybrid, PCG::PCGGenerationMode::Offline}) {
            auto draft = PCGGraphAuthoringDraft::Capture(document.Snapshot().Value(), catalog).Value();
            draft.values.mode = mode;
            const auto command = draft.Command(document.Snapshot().Value(), catalog, true);
            REQUIRE(command.HasValue());
            REQUIRE(document.Execute(command.Value()).HasValue());
            CHECK(document.Snapshot().Value().source->Data().mode == mode);
        }
    }
}  // namespace Horo::Editor
