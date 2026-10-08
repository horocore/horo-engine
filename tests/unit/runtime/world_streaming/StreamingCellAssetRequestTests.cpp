#include "Horo/Assets/AssetProvider.h"
#include "Horo/WorldStreaming/StreamingCellAssetRequest.h"
#include "Horo/WorldStreaming/StreamingCellDirection.h"
#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "StreamingCellCandidateTestSupport.h"
#include "WorldStreamingTestUtils.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <thread>
#include <type_traits>

namespace Horo::WorldStreaming {
    namespace {
        using namespace CandidateTestSupport;
        using TestSupport::Asset;
        using TestSupport::IdentityFrom;
        using TestSupport::RequireError;

        Assets::AssetRecord Record(const Assets::AssetId id, const std::size_t index) {
            const auto source = "assets/cell" + std::to_string(index) + ".bin";
            return {id, Assets::AssetTypeId::Parse("core.cell").Value(), ProjectPath::Parse(source).Value(),
                    ProjectPath::Parse(source + ".horo").Value()};
        }

        Assets::AssetRegistrySnapshot Registry(const std::span<const Assets::AssetId> ids) {
            Assets::AssetRegistry registry;
            std::vector<Assets::AssetRecord> records;
            for (std::size_t index{}; index < ids.size(); ++index)
                records.push_back(Record(ids[index], index));
            REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
            return registry.Snapshot();
        }

        StreamingCellAssetRequestContext AssetRequestContext() {
            return {IdentityFrom<StreamingCellAssetRequestId>(3), Operation(), 3, StreamingCellAssetRequestLifecycle::Active};
        }

        /** @brief Create the bounded scheduler used by the asset-retirement regression. */
        StreamingSchedulerAdmissionLedger RetirementScheduler() {
            auto result = StreamingSchedulerAdmissionLedger::Create(IdentityFrom<StreamingSchedulerLedgerId>(1),
                                                                    {1,
                                                                     5,
                                                                     {WorldPartitionProjectProfile::Editor,
                                                                      IdentityFrom<StreamingConcurrencyRevision>(1), 1, 1, 1}});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        struct RequestFixture final {
            std::array<Assets::AssetId, 3> ids{Asset(4), Asset(5), Asset(6)};
            CookedWorldIndexManifest manifest{Manifest()};
            StreamingCellCandidate candidate{Candidate(manifest)};
            Assets::AssetRegistrySnapshot registry{Registry(ids)};
        };

        /** @brief Exact release fixture shared by package admission, lifecycle and replacement regressions. */
        struct PackageRequestFixture final {
            RequestFixture cell;
            Assets::AssetChunkId base{Assets::AssetChunkId::Parse("base").Value()};
            Assets::AssetChunkId optional{Assets::AssetChunkId::Parse("optional").Value()};
            std::array<Assets::AssetChunkDefinition, 2>
                definitions{Assets::AssetChunkDefinition{base, Assets::AssetChunkKind::Base, {Asset(4), Asset(5)}, {}, 0, {}},
                            Assets::AssetChunkDefinition{optional, Assets::AssetChunkKind::Optional, {Asset(6)}, {base}, 1, {}}};
            Assets::AssetChunkPlan plan{Assets::AssetChunkPlan::Create(definitions).Value()};
            WorldPackageAssignmentBinding binding{IdentityFrom<WorldPackageAssignmentId>(1),
                                                  IdentityFrom<WorldPackageAssignmentRevision>(1), Operation().fence.epoch};
            WorldPackageChunkAssignment assignment{
                WorldPackageChunkAssignment::Create(cell.manifest, plan, binding, Hash(20), {3, 2, 3, 8}).Value()};
            WorldPackageAvailabilityRevision revision{IdentityFrom<WorldPackageAvailabilityRevision>(1)};
            WorldPackageContentContext context{binding, revision, WorldPackageContentLifecycle::Active};
            std::array<WorldPackageChunkAvailability, 2> installedStates{WorldPackageChunkAvailability{base,
                                                                                                       WorldPackageChunkState::Installed},
                                                                         WorldPackageChunkAvailability{optional,
                                                                                                       WorldPackageChunkState::Installed}};
            WorldPackageAvailabilitySnapshot installed{
                WorldPackageAvailabilitySnapshot::Create(assignment, revision, installedStates).Value()};
        };

        class LoadHarness final {
        public:
            LoadHarness(const Assets::IAssetProvider &provider, const std::size_t workers, const std::size_t maximumOutstanding = 128)
                : jobs_{JobSystemConfig{workers, 8}}, service_{jobs_, provider, maximumOutstanding} {}

            Assets::AssetLoadService &Service() noexcept {
                return service_;
            }

        private:
            JobSystem jobs_;
            Assets::AssetLoadService service_;
        };

        Result<StreamingCellAssetRequest> Submit(LoadHarness &loads, const RequestFixture &fixture) {
            return RequestStreamingCellAssets(loads.Service(), fixture.registry, fixture.manifest, fixture.candidate,
                                              AssetRequestContext());
        }

        void WaitTerminal(StreamingCellAssetRequest &request) {
            while (request.State() == StreamingCellAssetRequestState::Loading ||
                   request.State() == StreamingCellAssetRequestState::Cancelling)
                std::this_thread::yield();
        }

        /** @brief Keeps successful existence checks identical across delayed-load lifecycle fixtures. */
        class KnownAssetProvider : public Assets::IAssetProvider {
        public:
            Result<bool> Exists(Assets::AssetId, const CancellationToken &) const final {
                return Result<bool>::Success(true);
            }
        };

        class BlockingProvider final : public KnownAssetProvider {
        public:
            Result<std::vector<std::uint8_t>> Load(Assets::AssetId, const CancellationToken &cancellation) const override {
                entered.store(true);
                while (!cancellation.IsCancellationRequested())
                    std::this_thread::yield();
                return Result<std::vector<std::uint8_t>>::Failure(
                    Error{ErrorCode{"asset.load.cancelled"}, ErrorDomainId{"horo.asset"}, ErrorSeverity::Warning, "cancelled", {}});
            }

            mutable std::atomic<bool> entered{};
        };
    }  // namespace

    TEST_CASE("Cell asset request joins candidate and dependency bytes in canonical order", "[unit][world_streaming][asset_request]") {
        RequestFixture fixture;
        Assets::MemoryAssetProvider provider;
        provider.Insert(Asset(4), {4});
        provider.Insert(Asset(5), {5});
        provider.Insert(Asset(6), {6});
        LoadHarness loads{provider, 2};
        auto result = Submit(loads, fixture);
        REQUIRE(result.HasValue());
        auto request = std::move(result).Value();
        WaitTerminal(request);
        auto batch = request.TakeResult();
        REQUIRE(batch.HasValue());
        REQUIRE(batch.Value().request == AssetRequestContext().request);
        REQUIRE(batch.Value().operation == Operation());
        REQUIRE(batch.Value().registryRevision == fixture.registry.Revision());
        REQUIRE(batch.Value().assets.size() == 3);
        REQUIRE(batch.Value().assets[0].asset == Asset(4));
        REQUIRE((batch.Value().assets[0].bytes == std::vector<std::uint8_t>{4}));
        REQUIRE(batch.Value().assets[1].asset == Asset(5));
        REQUIRE(batch.Value().assets[2].asset == Asset(6));
        RequireError(request.TakeResult(), WorldStreamingErrors::CellAssetRequestConsumed);
        static_assert(!std::is_copy_constructible_v<StreamingCellAssetRequest>);
        static_assert(std::is_move_constructible_v<StreamingCellAssetRequest>);
    }

    TEST_CASE("Cell asset request rejects stale bounded and unavailable input transactionally",
              "[unit][world_streaming][asset_request][failure]") {
        RequestFixture fixture;
        Assets::MemoryAssetProvider provider;
        LoadHarness loads{provider, 1};
        auto context = AssetRequestContext();
        context.maximumRequests = 2;
        RequireError(RequestStreamingCellAssets(loads.Service(), fixture.registry, fixture.manifest, fixture.candidate, context),
                     WorldStreamingErrors::CellAssetRequestCapacityExceeded);
        context = AssetRequestContext();
        context.operation = Operation(IdentityFrom<StreamingGeneration>(2));
        RequireError(RequestStreamingCellAssets(loads.Service(), fixture.registry, fixture.manifest, fixture.candidate, context),
                     WorldStreamingErrors::CellAssetRequestStale);
        const std::array changedDependencies{Cell(1)};
        const auto changedManifest = Manifest(changedDependencies);
        context = AssetRequestContext();
        RequireError(RequestStreamingCellAssets(loads.Service(), fixture.registry, changedManifest, fixture.candidate, context),
                     WorldStreamingErrors::CellAssetRequestStale);
        const std::array missing{Asset(4), Asset(5)};
        fixture.registry = Registry(missing);
        context = AssetRequestContext();
        RequireError(RequestStreamingCellAssets(loads.Service(), fixture.registry, fixture.manifest, fixture.candidate, context),
                     WorldStreamingErrors::CellAssetRequestUnavailable);
        context.lifecycle = StreamingCellAssetRequestLifecycle::Closed;
        RequireError(RequestStreamingCellAssets(loads.Service(), fixture.registry, fixture.manifest, fixture.candidate, context),
                     WorldStreamingErrors::CellAssetRequestLifecycleUnavailable);
    }

    TEST_CASE("Cell asset request cancels partial admission when provider capacity rejects a child",
              "[unit][world_streaming][asset_request][capacity][cancellation]") {
        RequestFixture fixture;
        BlockingProvider provider;
        LoadHarness loads{provider, 1, 2};
        auto result = Submit(loads, fixture);
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == "asset.load.queue_full");
    }

    TEST_CASE("Cell asset request preserves provider failure precedence after cancellation",
              "[unit][world_streaming][asset_request][failure][cancellation]") {
        RequestFixture fixture;
        Assets::MemoryAssetProvider provider;
        provider.Insert(Asset(4), {4});
        provider.Insert(Asset(6), {6});
        LoadHarness loads{provider, 1};
        auto request = Submit(loads, fixture).Value();
        WaitTerminal(request);
        REQUIRE(request.State() == StreamingCellAssetRequestState::Failed);
        REQUIRE(request.RequestCancel().HasValue());
        REQUIRE(request.State() == StreamingCellAssetRequestState::Failed);
        const auto result = request.TakeResult();
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() != WorldStreamingErrors::CellAssetRequestCancelled.code.Value());
    }

    TEST_CASE("Cell asset request propagates cancellation to every child", "[unit][world_streaming][asset_request][cancellation]") {
        RequestFixture fixture;
        BlockingProvider provider;
        LoadHarness loads{provider, 1};
        auto result = Submit(loads, fixture);
        REQUIRE(result.HasValue());
        auto request = std::move(result).Value();
        while (!provider.entered.load())
            std::this_thread::yield();
        REQUIRE(request.RequestCancel().HasValue());
        WaitTerminal(request);
        REQUIRE(request.State() == StreamingCellAssetRequestState::Cancelled);
        REQUIRE(request.TakeResult().HasError());
    }

    TEST_CASE("Cell asset cancellation suppresses already ready bytes and leaves consumed success immutable",
              "[unit][world_streaming][asset_request][direction]") {
        RequestFixture fixture;
        Assets::MemoryAssetProvider provider;
        for (const auto id : fixture.ids)
            provider.Insert(id, {1});
        LoadHarness loads{provider, 1};
        auto request = Submit(loads, fixture).Value();
        WaitTerminal(request);
        REQUIRE(request.State() == StreamingCellAssetRequestState::Ready);
        REQUIRE(request.RequestCancel().HasValue());
        REQUIRE(request.State() == StreamingCellAssetRequestState::Cancelled);
        RequireError(request.TakeResult(), WorldStreamingErrors::CellAssetRequestCancelled);
        RequireError(request.TakeResult(), WorldStreamingErrors::CellAssetRequestConsumed);
        RequireError(request.RequestCancel(), WorldStreamingErrors::CellAssetRequestConsumed);

        auto succeeded = Submit(loads, fixture).Value();
        WaitTerminal(succeeded);
        REQUIRE(succeeded.TakeResult().HasValue());
        RequireError(succeeded.RequestCancel(), WorldStreamingErrors::CellAssetRequestConsumed);
        REQUIRE(succeeded.State() == StreamingCellAssetRequestState::Ready);
    }

    namespace {
        class DelayedProvider final : public KnownAssetProvider {
        public:
            Result<std::vector<std::uint8_t>> Load(Assets::AssetId, const CancellationToken &) const override {
                entered.store(true);
                while (!release.load())
                    std::this_thread::yield();
                return Result<std::vector<std::uint8_t>>::Success({1});
            }

            mutable std::atomic<bool> entered{};
            mutable std::atomic<bool> release{};
        };

        /** @brief Simulates successive owner frames while actual uncancellable reads drain in the regression. */
        Result<StreamingCellOperation> PollNextFrame(StreamingCellDirectionOwner &owner, const StreamingSchedulerAdmissionLedger &scheduler,
                                                     std::uint64_t &frame) {
            auto budget = StreamingOwnerFrameBudget::Create({scheduler.Owner(), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                                             IdentityFrom<StreamingOwnerFrameId>(++frame), 2000000, 1})
                              .Value();
            return owner.PollRetirement(budget, 0);
        }

        class AssetRetirementParticipant final : public IStreamingCellRetirementParticipant {
        public:
            explicit AssetRetirementParticipant(bool &published) : published_(published) {}

            void Start(StreamingCellAssetRequest request) {
                request_.emplace(std::move(request));
            }

            StreamingCellActivationRequirement Requirement() const noexcept override {
                return {IdentityFrom<StreamingRuntimeServiceId>(1), IdentityFrom<StreamingRuntimeServiceRevision>(1)};
            }

            StreamingCellOperationHandle Operation() const noexcept override {
                return CandidateTestSupport::Operation();
            }

            void RevokeAccess() noexcept override {
                if (request_)
                    static_cast<void>(request_->RequestCancel());
            }

            void BeginRetirement() noexcept override {}

            std::uint64_t MaximumRetirementNanoseconds() const noexcept override {
                return 1000000;
            }

            Result<std::optional<StreamingCellRetirementAcknowledgement>> PollRetirement() override {
                if (!request_)
                    return Result<std::optional<StreamingCellRetirementAcknowledgement>>::Success(
                        StreamingCellRetirementAcknowledgement{Requirement(), Operation()});
                const auto state = request_->State();
                if (state == StreamingCellAssetRequestState::Loading || state == StreamingCellAssetRequestState::Cancelling)
                    return Result<std::optional<StreamingCellRetirementAcknowledgement>>::Success(std::nullopt);
                if (!consumed_) {
                    auto result = request_->TakeResult();
                    published_ = result.HasValue();
                    consumed_ = true;
                }
                return Result<std::optional<StreamingCellRetirementAcknowledgement>>::Success(
                    StreamingCellRetirementAcknowledgement{Requirement(), Operation()});
            }

        private:
            bool &published_;
            std::optional<StreamingCellAssetRequest> request_;
            bool consumed_{};
        };

        struct ReleaseRead final {
            const DelayedProvider &provider;

            ~ReleaseRead() {
                provider.release.store(true);
            }
        };
    }  // namespace

    TEST_CASE("Direction reversal drains actual uncancellable asset work before releasing scheduler capacity",
              "[unit][world_streaming][asset_request][direction][lifecycle]") {
        RequestFixture fixture;
        DelayedProvider provider;
        LoadHarness loads{provider, 1};
        ReleaseRead release{provider};
        auto scheduler = RetirementScheduler();
        std::uint64_t frame{};
        const auto operation = StreamingCellOperation::Create(CandidateTestSupport::Operation(), StreamingCellOperationKind::Load).Value();
        std::vector<std::unique_ptr<IStreamingCellRetirementParticipant>> participants;
        bool published{};
        auto adapter = std::make_unique<AssetRetirementParticipant>(published);
        auto &assets = *adapter;
        participants.push_back(std::move(adapter));
        auto owner = StreamingCellDirectionOwner::Create(scheduler,
                                                         {operation, IdentityFrom<StreamingCellDemandRevision>(1),
                                                          StreamingDesiredResidency::Loaded, 1, 5},
                                                         participants)
                         .Value();
        REQUIRE(owner.Advance(owner.Operation(), StreamingCellOperationTransition::BeginPreparation).HasValue());
        REQUIRE(scheduler.ReservedCapacityUnits() == 5);
        assets.Start(Submit(loads, fixture).Value());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!provider.entered.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE(provider.entered.load());
        REQUIRE(owner
                    .UpdateDemand(operation.Handle(), IdentityFrom<StreamingCellDemandRevision>(1),
                                  IdentityFrom<StreamingCellDemandRevision>(2), StreamingDesiredResidency::Unloaded)
                    .HasValue());
        REQUIRE(PollNextFrame(owner, scheduler, frame).Value().State() == StreamingCellOperationState::Retiring);
        REQUIRE(scheduler.ReservedCapacityUnits() == 5);
        REQUIRE(owner
                    .UpdateDemand(operation.Handle(), IdentityFrom<StreamingCellDemandRevision>(2),
                                  IdentityFrom<StreamingCellDemandRevision>(3), StreamingDesiredResidency::Loaded)
                    .HasValue());
        provider.release.store(true);
        const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!owner.Operation().IsTerminal() && std::chrono::steady_clock::now() < drainDeadline) {
            REQUIRE(PollNextFrame(owner, scheduler, frame).HasValue());
            std::this_thread::yield();
        }
        REQUIRE(owner.Operation().IsTerminal());
        REQUIRE(owner.TakeTerminalResult().Value().Outcome() == StreamingCellOperationOutcome::Cancelled);
        REQUIRE(scheduler.ReservedCapacityUnits() == 0);
        REQUIRE(owner.RequiresFreshAttempt());
        REQUIRE_FALSE(published);
    }

    TEST_CASE("Packaged cell reads reject missing content before any child admission",
              "[unit][world_streaming][asset_request][package_chunk]") {
        PackageRequestFixture package;
        auto states = package.installedStates;
        states[1].state = WorldPackageChunkState::Downloadable;
        const auto missing = WorldPackageAvailabilitySnapshot::Create(package.assignment, package.revision, states).Value();
        BlockingProvider blocked;
        LoadHarness loads{blocked, 1};
        RequireError(RequestStreamingCellAssets(loads.Service(), package.cell.registry, package.cell.manifest, package.cell.candidate,
                                                AssetRequestContext(), {package.assignment, missing, package.context}),
                     WorldStreamingErrors::PackageChunkContentMissing);
        REQUIRE_FALSE(blocked.entered.load());
    }

    TEST_CASE("Packaged cell reads preserve ordinary request cancellation and shutdown admission",
              "[unit][world_streaming][asset_request][package_chunk]") {
        PackageRequestFixture package;
        Assets::MemoryAssetProvider provider;
        for (const auto id : package.cell.ids)
            provider.Insert(id, {1});
        LoadHarness loads{provider, 1};
        auto context = package.context;
        context.lifecycle = WorldPackageContentLifecycle::Closed;
        RequireError(RequestStreamingCellAssets(loads.Service(), package.cell.registry, package.cell.manifest, package.cell.candidate,
                                                AssetRequestContext(), {package.assignment, package.installed, context}),
                     WorldStreamingErrors::PackageChunkLifecycleUnavailable);
        auto admitted = RequestStreamingCellAssets(loads.Service(), package.cell.registry, package.cell.manifest, package.cell.candidate,
                                                   AssetRequestContext(), {package.assignment, package.installed, package.context});
        REQUIRE(admitted.HasValue());
        auto request = std::move(admitted).Value();
        WaitTerminal(request);
        REQUIRE(request.RequestCancel().HasValue());
        RequireError(request.TakeResult(), WorldStreamingErrors::CellAssetRequestCancelled);
        auto success = RequestStreamingCellAssets(loads.Service(), package.cell.registry, package.cell.manifest, package.cell.candidate,
                                                  AssetRequestContext(), {package.assignment, package.installed, package.context})
                           .Value();
        WaitTerminal(success);
        REQUIRE(success.TakeResult().Value().assets.size() == 3);
    }

    TEST_CASE("Packaged cell reads reject replaced epochs and exact artifact metadata",
              "[unit][world_streaming][asset_request][package_chunk]") {
        PackageRequestFixture package;
        Assets::MemoryAssetProvider provider;
        LoadHarness loads{provider, 1};
        auto foreignBinding = package.binding;
        foreignBinding.epoch = IdentityFrom<PartitionEpoch>(2);
        const auto foreign =
            WorldPackageChunkAssignment::Create(package.cell.manifest, package.plan, foreignBinding, Hash(20), {3, 2, 3, 8}).Value();
        const auto foreignSnapshot = WorldPackageAvailabilitySnapshot::Create(foreign, package.revision, package.installedStates).Value();
        RequireError(RequestStreamingCellAssets(loads.Service(), package.cell.registry, package.cell.manifest, package.cell.candidate,
                                                AssetRequestContext(),
                                                {foreign,
                                                 foreignSnapshot,
                                                 {foreignBinding, package.revision, WorldPackageContentLifecycle::Active}}),
                     WorldStreamingErrors::PackageChunkStale);
        const auto &descriptor = package.cell.manifest.Descriptor();
        auto cloned = WorldPartitionDescriptor::Create(descriptor.Version(), descriptor.Partition(), descriptor.Bounds(), descriptor.Grid(),
                                                       descriptor.Layers(), descriptor.Cells(), {2, 4, 16})
                          .Value();
        std::vector<CookedWorldCellManifestCandidate> changedCells;
        for (std::size_t index{}; index < package.cell.manifest.Cells().size(); ++index) {
            const auto &cell = package.cell.manifest.Cells()[index];
            changedCells.push_back({cell.cell, cell.uncompressedSize, cell.compressedSize, cell.payloadCrc32,
                                    index == 0 ? Hash(30) : cell.artifactHash, package.cell.manifest.HardDependencies(index)});
        }
        const auto changedManifest = CookedWorldIndexManifest::Create(std::move(cloned), changedCells, {4, 4, 4, 256, 256}).Value();
        const auto changed =
            WorldPackageChunkAssignment::Create(changedManifest, package.plan, package.binding, Hash(20), {3, 2, 3, 8}).Value();
        const auto changedSnapshot = WorldPackageAvailabilitySnapshot::Create(changed, package.revision, package.installedStates).Value();
        RequireError(RequestStreamingCellAssets(loads.Service(), package.cell.registry, package.cell.manifest, package.cell.candidate,
                                                AssetRequestContext(), {changed, changedSnapshot, package.context}),
                     WorldStreamingErrors::PackageChunkStale);
    }
}  // namespace Horo::WorldStreaming
