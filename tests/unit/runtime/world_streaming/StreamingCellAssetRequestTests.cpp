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

        struct RequestFixture final {
            std::array<Assets::AssetId, 3> ids{Asset(4), Asset(5), Asset(6)};
            CookedWorldIndexManifest manifest{Manifest()};
            StreamingCellCandidate candidate{Candidate(manifest)};
            Assets::AssetRegistrySnapshot registry{Registry(ids)};
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

        class BlockingProvider final : public Assets::IAssetProvider {
        public:
            Result<bool> Exists(Assets::AssetId, const CancellationToken &) const override {
                return Result<bool>::Success(true);
            }

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
        class DelayedProvider final : public Assets::IAssetProvider {
        public:
            Result<bool> Exists(Assets::AssetId, const CancellationToken &) const override {
                return Result<bool>::Success(true);
            }

            Result<std::vector<std::uint8_t>> Load(Assets::AssetId, const CancellationToken &) const override {
                entered.store(true);
                while (!release.load())
                    std::this_thread::yield();
                return Result<std::vector<std::uint8_t>>::Success({1});
            }

            mutable std::atomic<bool> entered{};
            mutable std::atomic<bool> release{};
        };

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
        auto schedulerResult = StreamingSchedulerAdmissionLedger::Create(IdentityFrom<StreamingSchedulerLedgerId>(1),
                                                                         {1,
                                                                          5,
                                                                          {WorldPartitionProjectProfile::Editor,
                                                                           IdentityFrom<StreamingConcurrencyRevision>(1), 1, 1, 1}});
        REQUIRE(schedulerResult.HasValue());
        auto scheduler = std::move(schedulerResult).Value();
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
        REQUIRE(owner.PollRetirement().Value().State() == StreamingCellOperationState::Retiring);
        REQUIRE(scheduler.ReservedCapacityUnits() == 5);
        REQUIRE(owner
                    .UpdateDemand(operation.Handle(), IdentityFrom<StreamingCellDemandRevision>(2),
                                  IdentityFrom<StreamingCellDemandRevision>(3), StreamingDesiredResidency::Loaded)
                    .HasValue());
        provider.release.store(true);
        const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!owner.Operation().IsTerminal() && std::chrono::steady_clock::now() < drainDeadline) {
            REQUIRE(owner.PollRetirement().HasValue());
            std::this_thread::yield();
        }
        REQUIRE(owner.Operation().IsTerminal());
        REQUIRE(owner.TakeTerminalResult().Value().Outcome() == StreamingCellOperationOutcome::Cancelled);
        REQUIRE(scheduler.ReservedCapacityUnits() == 0);
        REQUIRE(owner.RequiresFreshAttempt());
        REQUIRE_FALSE(published);
    }
}  // namespace Horo::WorldStreaming
