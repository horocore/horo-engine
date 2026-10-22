#include "Horo/Editor/WorldStreamingPreview.h"
#include "StreamingPlannerFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
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
                desired.front().stability = {.policy = Id<WS::StreamingCellStabilityPolicyId>(),
                                             .policyRevision = Id<WS::StreamingCellStabilityPolicyRevision>(),
                                             .partition = world,
                                             .epoch = owner.epoch,
                                             .cell = desired.front().cell.cell};
            }

            WS::StreamingCellStateRecord FailedAttempt() const {
                const WS::StreamingCellOperationHandle handle{Id<WS::StreamingCellOperationId>(),
                                                              {world, owner.epoch, desired.front().cell.cell,
                                                               Id<WS::StreamingGeneration>()}};
                auto ledger = WS::StreamingCellStateLedger::Create({owner, 4}).Value();
                auto operation = WS::StreamingCellOperation::Create(handle, WS::StreamingCellOperationKind::Load).Value();
                operation = operation.Advance(handle, WS::StreamingCellOperationTransition::Admit).Value();
                REQUIRE(ledger.Apply(owner, operation).Value().state == WS::StreamingCellState::Loading);
                operation = operation.Advance(handle, WS::StreamingCellOperationTransition::Fail).Value();
                REQUIRE(ledger.Apply(owner, operation).Value().state == WS::StreamingCellState::Evicting);
                operation = operation.Advance(handle, WS::StreamingCellOperationTransition::AcknowledgeRetirement).Value();
                return ledger.Apply(owner, operation).Value();
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
                const std::array attempts{FailedAttempt()};
                const std::array failures{
                    WS::StreamingDiagnosticFailureRecord{attempts.front().operation, WS::StreamingCellOperationOutcome::Failed}};
                auto result =
                    WS::WorldStreamingDiagnosticSnapshot::Create(input, policy, sample, {.cells = attempts, .failures = failures});
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            WS::StreamingSourcePlanView Plan() const {
                return {.partition = snapshot.Binding(),
                        .revision = 1,
                        .sampledAtServiceMilliseconds = 10,
                        .cells = desired,
                        .contributions = contributions};
            }

            WorldStreamingPreviewFence Fence() const {
                return {source->fence, snapshot.Binding(), Id<WS::WorldStreamingDiagnosticRevision>(), 1, 1};
            }
        };

        struct NativeFixture : Fixture {
            std::unique_ptr<WS::StreamingSourcePlanner> planner;
            WS::StreamingSourceRegistration registration;
            WS::WorldStreamingDiagnosticSnapshot diagnostics = Diagnostics();

            NativeFixture() {
                const std::array enabled{layer};
                const WS::StreamingSourceEnvironment environment{1, {1}, 1, 0, enabled};
                planner = WS::StreamingSourcePlanner::Create(WS::PlannerFixture::Config(4), snapshot, environment).Value();
                const WS::StreamingSourceRequest request{.id = Id<WS::StreamingSourceId>(),
                                                         .revision = Id<WS::StreamingSourceRevision>(),
                                                         .intent = WS::StreamingSourceIntent::Gameplay,
                                                         .priority = WS::StreamingSourcePriority::Create(1.0F).Value(),
                                                         .shape =
                                                             WS::StreamingSourceBox{Math::WorldCoordinate64::FromMillimeters(-1000, 0, 0),
                                                                                    Math::WorldCoordinate64::FromMillimeters(-1, 999, 999)},
                                                         .residency = WS::StreamingDesiredResidency::Activated};
                registration = planner->Register(request).Value();
                REQUIRE_FALSE(WS::PlannerFixture::Finish(*planner).cells.empty());
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

        TEST_CASE("Streaming preview rejects malformed desired provenance", "[unit][editor][streaming_preview]") {
            Fixture fixture;
            const auto diagnostics = fixture.Diagnostics();
            std::string_view error = "world_document.invalid";
            SECTION("invalid priority source") {
                fixture.desired.front().prioritySource = WS::StreamingSourceDescriptor{};
            }
            SECTION("stale stability cell") {
                ++fixture.desired.front().stability.cell.x;
                error = "world_document.stale";
            }
            SECTION("invalid stability policy") {
                fixture.desired.front().stability.policy = {};
            }
            SECTION("future desired time") {
                fixture.desired.front().queuedAtServiceMilliseconds = 11;
                error = "world_document.stale";
            }
            SECTION("pin exceeds desired residency") {
                fixture.desired.front().residency = WS::StreamingDesiredResidency::Loaded;
                fixture.desired.front().pinnedFloor = WS::StreamingDesiredResidency::Activated;
            }
            RequireError(WorldStreamingPreview::Create(fixture.source, fixture.snapshot, fixture.Plan(), diagnostics, fixture.Fence()),
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

        TEST_CASE("Streaming geometry uses the canonical origin and rejects stale capacity and cancellation",
                  "[unit][editor][streaming_preview]") {
            Fixture fixture;
            const auto diagnostics = fixture.Diagnostics();
            auto preview = WorldStreamingPreview::Create(fixture.source, fixture.snapshot, fixture.Plan(), diagnostics, fixture.Fence());
            REQUIRE(preview.HasValue());
            const auto owned = std::make_shared<const WorldStreamingPreview>(preview.Value());
            WorldViewportBinding view{fixture.source, {}, 1};
            const auto geometry = BuildWorldStreamingPreviewGeometry(owned, view);
            REQUIRE(geometry.HasValue());
            REQUIRE(geometry.Value()->boxes.size() == 2);
            CHECK(geometry.Value()->boxes.front().corners.front().x == -1.0F);
            RequireError(BuildWorldStreamingPreviewGeometry(owned, view, 1), "world_document.capacity");
            view.viewRevision = 2;
            RequireError(BuildWorldStreamingPreviewGeometry(owned, view), "world_document.stale");
            view.viewRevision = 1;
            std::stop_source cancellation;
            cancellation.request_stop();
            RequireError(BuildWorldStreamingPreviewGeometry(owned, view, 256, cancellation.get_token()), "world_document.cancelled");
            view.globalOrigin = Math::WorldCoordinate64::FromMillimeters(100'000'000, 0, 0);
            const auto distant = BuildWorldStreamingPreviewGeometry(owned, view);
            REQUIRE(distant.HasValue());
            CHECK(distant.Value()->boxes.empty());
            CHECK(distant.Value()->outsideLocalWindow == 2);
        }

        TEST_CASE("Native streaming preview bridge reads real planner publications and revokes cached evidence on unmount",
                  "[unit][editor][streaming_preview][native_host]") {
            NativeFixture fixture;
            WorldStreamingPreviewHost host;
            const WorldViewportBinding view{fixture.source, {}, 1};
            RequireError(host.Refresh(view), "world_document.lifecycle");
            REQUIRE(host.BindRuntime(*fixture.registry, *fixture.planner, fixture.diagnostics).HasValue());
            const auto first = host.Refresh(view);
            REQUIRE(first.HasValue());
            REQUIRE(first.Value()->preview->Cells().front().desired.has_value());
            CHECK(first.Value()->preview->Cells().front().desired->residency == WS::StreamingDesiredResidency::Activated);
            CHECK(first.Value()->preview->Cells().front().attempts.front().state == WS::StreamingCellState::Failed);
            CHECK(host.Refresh(view).Value() == first.Value());
            auto stale = fixture.owner;
            stale.epoch = Id<WS::PartitionEpoch>(2);
            RequireError(host.UnbindRuntime(stale), "world_document.stale");
            REQUIRE(host.UnbindRuntime(fixture.owner).HasValue());
            RequireError(host.Refresh(view), "world_document.lifecycle");
            CHECK(first.Value()->preview->Cells().front().attempts.front().state == WS::StreamingCellState::Failed);
            CHECK(fixture.registration.Close().HasValue());
        }

        TEST_CASE("Native streaming capture failures revoke current presentation without destroying historical leases",
                  "[unit][editor][streaming_preview][native_host]") {
            NativeFixture fixture;
            WorldStreamingPreviewHost host;
            const WorldViewportBinding view{fixture.source, {}, 1};
            REQUIRE(host.BindRuntime(*fixture.registry, *fixture.planner, fixture.diagnostics).HasValue());
            const auto historical = host.Refresh(view).Value();
            SECTION("planner cancellation") {
                REQUIRE(fixture.planner->Shutdown().HasValue());
                CHECK(host.Refresh(view).HasError());
            }
            SECTION("source withdrawal invalidates published intent") {
                REQUIRE(fixture.registration.Close().HasValue());
                CHECK(host.Refresh(view).HasError());
            }
            SECTION("closed runtime diagnostic publication") {
                fixture.diagnostics = fixture.Diagnostics(WS::WorldStreamingRuntimeCompositionState::Closed);
                RequireError(host.Refresh(view), "world_document.lifecycle");
            }
            SECTION("registry shutdown") {
                fixture.registry->Shutdown();
                CHECK(host.Refresh(view).HasError());
            }
            SECTION("changed origin cannot reuse a view revision") {
                auto moved = view;
                moved.globalOrigin = Math::WorldCoordinate64::FromMillimeters(1000, 0, 0);
                RequireError(host.Refresh(moved), "world_document.stale");
                moved.viewRevision = 2;
                REQUIRE(host.Refresh(moved).HasValue());
                RequireError(host.Refresh(view), "world_document.stale");
            }
            CHECK(historical->preview->Cells().front().attempts.front().state == WS::StreamingCellState::Failed);
            REQUIRE(host.UnbindRuntime(fixture.owner).HasValue());
        }

        TEST_CASE("Native capture rejects access outside the native owner lane", "[unit][editor][streaming_preview][native_host]") {
            NativeFixture fixture;
            WorldStreamingPreviewHost host;
            const WorldViewportBinding view{fixture.source, {}, 1};
            REQUIRE(host.BindRuntime(*fixture.registry, *fixture.planner, fixture.diagnostics).HasValue());
            bool rejected{};
            std::thread thread{[&] {
                const auto result = host.Refresh(view);
                rejected = result.HasError() && result.ErrorValue().code.Value() == WorldDocumentErrors::WrongThread.code.Value();
            }};
            thread.join();
            CHECK(rejected);
            REQUIRE(host.UnbindRuntime(fixture.owner).HasValue());
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
