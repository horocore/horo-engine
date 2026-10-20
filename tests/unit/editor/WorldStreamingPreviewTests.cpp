#include "Horo/Editor/WorldStreamingPreview.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;

        template <typename T> T Id(const std::uint64_t value = 1) {
            return T::Create(value).Value();
        }

        Assets::AssetId Asset(const std::uint8_t value = 1) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = value;
            return Assets::AssetId::FromBytes(bytes);
        }

        struct Fixture {
            WS::WorldPartitionId world = WS::WorldPartitionId::Create(Asset().Bytes()).Value();
            WS::StreamingRuntimeOwnerToken owner{world, Id<WS::PartitionEpoch>(), Id<WS::StreamingRuntimeOwnerId>()};
            WS::WorldCellQuantizationPolicy grid = WS::WorldCellQuantizationPolicy::Create({}, 1000, {-1, 1, -1, 1, -1, 1}, 1).Value();
            WS::StreamingLayerId layer = WS::StreamingLayerId::Create(0).Value();
            PartitionedWorldWorkspace workspace{Asset(9)};
            PartitionedWorldSnapshotLease source;
            std::unique_ptr<WS::WorldPartitionRegistry> registry;
            WS::WorldPartitionRegistrySnapshot snapshot;
            std::vector<WS::StreamingPlannedCell> desired;
            std::vector<WS::StreamingPlannedSourceCell> contributions;

            Fixture() {
                PartitionedWorldDocument document{.settings = {.partition = world, .bounds = {}, .grid = grid}};
                document.layers.push_back({.id = layer, .name = "Terrain"});
                source = workspace.Open(document).Value();
                registry = std::move(WS::WorldPartitionRegistry::Create(Id<WS::WorldPartitionRegistryId>(), owner, {4, 4})).Value();
                const std::array cells{WS::WorldPartitionCellDescriptor{{-1, 0, 0, 0, layer}, {Asset(2)}},
                                       WS::WorldPartitionCellDescriptor{{0, 0, 0, 0, layer}, {Asset(3)}}};
                auto descriptor = WS::WorldPartitionDescriptor::Create({}, world, {}, grid, document.layers, cells, {4, 4, 64});
                REQUIRE(descriptor.HasValue());
                REQUIRE(registry->Publish(std::move(descriptor).Value(), Id<WS::WorldPartitionRegistryRevision>()).HasValue());
                snapshot = registry->Snapshot().Value();
                desired.push_back({.cell = snapshot.Find(cells.front().id).Value(), .residency = WS::StreamingDesiredResidency::Activated});
            }

            WS::WorldStreamingDiagnosticSnapshot Diagnostics(
                const WS::WorldStreamingRuntimeCompositionState lifecycle = WS::WorldStreamingRuntimeCompositionState::Active) const {
                std::array<WS::StreamingBudgetLimit, WS::StreamingBudgetDimensionCount> limits{};
                std::array<WS::StreamingBudgetAmount, WS::StreamingBudgetDimensionCount> usage{};
                for (std::size_t index = 0; index < limits.size(); ++index) {
                    limits[index] = {.dimension = static_cast<WS::StreamingBudgetDimension>(index), .softTarget = 50, .hardLimit = 100};
                    usage[index] = {.dimension = static_cast<WS::StreamingBudgetDimension>(index), .value = 0};
                }
                const auto policy =
                    WS::StreamingBudgetPolicy::Create(Id<WS::StreamingBudgetPolicyRevision>(), limits, std::chrono::seconds{1}).Value();
                const auto sample =
                    WS::StreamingBudgetSample::Create(policy, Id<WS::StreamingBudgetSampleRevision>(), std::chrono::nanoseconds{0},
                                                      std::chrono::nanoseconds{10}, WS::StreamingBudgetAmounts::Create(usage).Value())
                        .Value();
                WS::WorldStreamingDiagnosticSnapshotInput
                    input{.owner = owner,
                          .revision = Id<WS::WorldStreamingDiagnosticRevision>(),
                          .ownerRevision = Id<WS::StreamingRuntimeCompositionRevision>(),
                          .lifecycle = lifecycle,
                          .instrumentation = WS::StreamingDiagnosticInstrumentation::Disabled,
                          .limits = {.sources = 4, .cells = 4, .failures = 4},
                          .queue = {.owner = Id<WS::StreamingSchedulerLedgerId>(),
                                    .limits = {.concurrentOperations = 4,
                                               .capacityUnits = 100,
                                               .concurrency = {.profile = WS::WorldPartitionProjectProfile::Editor,
                                                               .revision = Id<WS::StreamingConcurrencyRevision>(),
                                                               .loads = 4,
                                                               .activations = 4,
                                                               .retirements = 4}},
                                    .state = WS::StreamingSchedulerAdmissionState::Accepting}};
                if (lifecycle == WS::WorldStreamingRuntimeCompositionState::Closed)
                    input.queue.state = WS::StreamingSchedulerAdmissionState::Closed;
                const WS::StreamingCellOperationHandle operation{Id<WS::StreamingCellOperationId>(),
                                                                 {world, owner.epoch, desired.front().cell.cell,
                                                                  Id<WS::StreamingGeneration>()}};
                const std::array attempts{WS::StreamingCellStateRecord{operation, WS::StreamingCellState::Failed}};
                const std::array failures{WS::StreamingDiagnosticFailureRecord{operation, WS::StreamingCellOperationOutcome::Failed}};
                auto result =
                    WS::WorldStreamingDiagnosticSnapshot::Create(input, policy, sample, {.cells = attempts, .failures = failures});
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            WS::StreamingSourcePlanView Plan() const {
                return {.partition = snapshot.Binding(), .revision = 1, .cells = desired, .contributions = contributions};
            }

            WorldStreamingPreviewFence Fence() const {
                return {source->fence, snapshot.Binding(), Id<WS::WorldStreamingDiagnosticRevision>(), 1, 1};
            }
        };

        template <typename T> void RequireError(const Result<T> &result, const std::string_view code) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == code);
        }

        TEST_CASE("Streaming preview keeps desired state distinct from actual failed residency and absent demand",
                  "[unit][editor][streaming_preview]") {
            Fixture fixture;
            const auto diagnostics = fixture.Diagnostics();
            const auto result =
                WorldStreamingPreview::Create(fixture.source, fixture.snapshot, fixture.Plan(), diagnostics, fixture.Fence());
            REQUIRE(result.HasValue());
            const auto &preview = result.Value();
            REQUIRE(preview.Cells().size() == 2);
            const auto &first = preview.Cells().front();
            REQUIRE(first.cell.cell.x == -1);
            REQUIRE(first.desired->residency == WS::StreamingDesiredResidency::Activated);
            REQUIRE(first.attempts.front().state == WS::StreamingCellState::Failed);
            REQUIRE(first.failures.front().reason == WS::StreamingCellOperationOutcome::Failed);
            REQUIRE(first.decisions.empty());
            REQUIRE(first.bounds.minimum.Millimeters()[0] == -1000);
            REQUIRE(first.bounds.maximum.Millimeters()[0] == -1);
            REQUIRE_FALSE(preview.Cells().back().desired.has_value());
            REQUIRE(preview.Cells().back().attempts.empty());
            REQUIRE(fixture.workspace.Snapshot().Value() == fixture.source);
            REQUIRE_FALSE(fixture.source->dirty);
        }

        TEST_CASE("Streaming preview rejects stale publications atomically", "[unit][editor][streaming_preview]") {
            Fixture fixture;
            const auto diagnostics = fixture.Diagnostics();
            auto fence = fixture.Fence();
            SECTION("document revision") {
                fence.document.revision = Id<WorldDocumentRevision>(2);
            }
            SECTION("registry revision") {
                fence.registry.revision = Id<WS::WorldPartitionRegistryRevision>(2);
            }
            SECTION("diagnostics revision") {
                fence.diagnostics = Id<WS::WorldStreamingDiagnosticRevision>(2);
            }
            SECTION("plan revision") {
                fence.planRevision = 2;
            }
            RequireError(WorldStreamingPreview::Create(fixture.source, fixture.snapshot, fixture.Plan(), diagnostics, fence),
                         "world_document.stale");
            REQUIRE(fixture.workspace.Snapshot().Value() == fixture.source);
        }

        TEST_CASE("Streaming preview rejects invalid unsupported duplicate cancelled and capacity inputs",
                  "[unit][editor][streaming_preview]") {
            Fixture fixture;
            const auto diagnostics = fixture.Diagnostics();
            auto fence = fixture.Fence();
            auto limits = WorldStreamingPreviewLimits{};
            std::stop_source cancellation;
            std::string_view error = "world_document.invalid";
            SECTION("missing view publication") {
                fence.viewRevision = 0;
            }
            SECTION("cell ceiling") {
                limits.cells = 1;
                error = "world_document.capacity";
            }
            SECTION("zero attempts ceiling") {
                limits.attempts = 0;
                error = "world_document.capacity";
            }
            SECTION("hard decision ceiling") {
                limits.decisions = 4097;
                error = "world_document.capacity";
            }
            SECTION("duplicate desired cell") {
                fixture.desired.push_back(fixture.desired.front());
                error = "world_document.conflict";
            }
            SECTION("unsupported desired residency") {
                fixture.desired.front().residency = static_cast<WS::StreamingDesiredResidency>(255);
                error = "world_document.unsupported";
            }
            SECTION("cancellation") {
                cancellation.request_stop();
                error = "world_document.cancelled";
            }
            RequireError(WorldStreamingPreview::Create(fixture.source, fixture.snapshot, fixture.Plan(), diagnostics, fence, limits,
                                                       cancellation.get_token()),
                         error);
            REQUIRE(fixture.workspace.Snapshot().Value() == fixture.source);
        }

        TEST_CASE("Streaming preview retains current and predicted source reasons without borrowing", "[unit][editor][streaming_preview]") {
            Fixture fixture;
            const auto diagnostics = fixture.Diagnostics();
            const WS::StreamingSourceDescriptor source{.id = Id<WS::StreamingSourceId>(),
                                                       .owner = {.partition = fixture.world,
                                                                 .epoch = fixture.owner.epoch,
                                                                 .slot = 0,
                                                                 .generation = 1},
                                                       .intent = WS::StreamingSourceIntent::Camera,
                                                       .priority = WS::StreamingSourcePriority::Create(1.0F).Value(),
                                                       .revision = Id<WS::StreamingSourceRevision>()};
            fixture.contributions.push_back({fixture.desired.front().cell, source, WS::StreamingDesiredResidency::Activated,
                                             WS::StreamingRetention::Pinned, false, 10});
            fixture.contributions.push_back({fixture.desired.front().cell, source, WS::StreamingDesiredResidency::Loaded,
                                             WS::StreamingRetention::Releasable, true, 10});
            auto result = WorldStreamingPreview::Create(fixture.source, fixture.snapshot, fixture.Plan(), diagnostics, fixture.Fence());
            REQUIRE(result.HasValue());
            fixture.contributions.push_back(fixture.contributions.front());
            RequireError(WorldStreamingPreview::Create(fixture.source, fixture.snapshot, fixture.Plan(), diagnostics, fixture.Fence()),
                         "world_document.conflict");
            fixture.contributions.clear();
            REQUIRE(result.Value().Cells().front().contributions.size() == 2);
            REQUIRE(result.Value().Cells().front().contributions.back().predicted);
        }

        TEST_CASE("Streaming preview owns borrowed plan data and survives source shutdown", "[unit][editor][streaming_preview]") {
            Fixture fixture;
            const auto diagnostics = fixture.Diagnostics();
            auto result = WorldStreamingPreview::Create(fixture.source, fixture.snapshot, fixture.Plan(), diagnostics, fixture.Fence());
            REQUIRE(result.HasValue());
            const auto oldFence = result.Value().Fence();
            fixture.desired.clear();
            REQUIRE(fixture.workspace.Shutdown().HasValue());
            REQUIRE(result.Value().Cells().front().desired->residency == WS::StreamingDesiredResidency::Activated);
            REQUIRE(result.Value().Source()->fence == oldFence.document);
        }

        TEST_CASE("Streaming preview rejects terminal runtime instead of fabricating empty residency",
                  "[unit][editor][streaming_preview]") {
            Fixture fixture;
            const auto diagnostics = fixture.Diagnostics(WS::WorldStreamingRuntimeCompositionState::Closed);
            RequireError(WorldStreamingPreview::Create(fixture.source, fixture.snapshot, fixture.Plan(), diagnostics, fixture.Fence()),
                         "world_document.lifecycle");
        }
    }  // namespace

    static_assert(
        std::is_same_v<decltype(std::declval<const WorldStreamingPreview &>().Cells()), std::span<const WorldStreamingPreviewCell>>);
}  // namespace Horo::Editor
