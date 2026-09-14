#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "Horo/WorldStreaming/WorldStreamingTrace.h"
#include "WorldStreamingTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::Layer;
        using TestSupport::RequireError;
        using TestSupport::World;

        class SpanSink final : public Telemetry::ISink {
        public:
            void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *) override {
                if (record.Kind() != Telemetry::RecordKind::Span)
                    return;
                std::scoped_lock lock{mutex_};
                records_.push_back(record);
            }

            void Flush() override {}

            [[nodiscard]] std::vector<Telemetry::Record> Records() const {
                std::scoped_lock lock{mutex_};
                return records_;
            }

        private:
            mutable std::mutex mutex_;
            std::vector<Telemetry::Record> records_;
        };

        class TraceTelemetry final {
        public:
            TraceTelemetry() {
                static_cast<void>(Telemetry::Runtime::Shutdown());
                sink = std::make_shared<SpanSink>();
                REQUIRE(Telemetry::Runtime::Initialize({.queueCapacity = 32}, sink));
            }

            ~TraceTelemetry() {
                static_cast<void>(Telemetry::Runtime::Shutdown());
            }

            std::shared_ptr<SpanSink> sink;
        };

        [[nodiscard]] StreamingRuntimeOwnerToken Owner(const std::uint64_t id = 9) {
            return {.partition = World(), .epoch = IdentityFrom<PartitionEpoch>(3), .owner = IdentityFrom<StreamingRuntimeOwnerId>(id)};
        }

        [[nodiscard]] StreamingCellOperationHandle Operation(const std::uint64_t id = 17) {
            return {.operation = IdentityFrom<StreamingCellOperationId>(id),
                    .fence = {.partition = World(),
                              .epoch = IdentityFrom<PartitionEpoch>(3),
                              .cell = {.x = 1, .y = 2, .z = 3, .lod = 0, .layer = Layer()},
                              .generation = IdentityFrom<StreamingGeneration>(4)}};
        }

        [[nodiscard]] StreamingTraceConfiguration Configuration(const std::uint64_t revision = 2, const std::size_t capacity = 8) {
            return {.owner = Owner(),
                    .ownerRevision = IdentityFrom<StreamingRuntimeCompositionRevision>(5),
                    .bindingRevision = IdentityFrom<StreamingTraceBindingRevision>(revision),
                    .operation = IdentityFrom<StreamingTraceOperationId>(101),
                    .maximumSpans = capacity};
        }

        [[nodiscard]] StreamingTraceStageBegin SourceStage(const std::uint64_t span = 201) {
            return {.span = IdentityFrom<StreamingTraceSpanId>(span),
                    .parentRoot = IdentityFrom<StreamingTraceOperationId>(101),
                    .stage = StreamingTraceStage::SourceEvaluation,
                    .subject = {.source = IdentityFrom<StreamingSourceId>(23)}};
        }

        [[nodiscard]] StreamingTraceStageBegin AssetStage(const std::uint64_t span = 202, const std::uint64_t parent = 201) {
            return {.span = IdentityFrom<StreamingTraceSpanId>(span),
                    .parentRoot = IdentityFrom<StreamingTraceOperationId>(101),
                    .parentSpan = IdentityFrom<StreamingTraceSpanId>(parent),
                    .stage = StreamingTraceStage::AssetRequest,
                    .subject = {.cellOperation = Operation(), .assetRequest = IdentityFrom<StreamingCellAssetRequestId>(31)}};
        }

        [[nodiscard]] StreamingTraceStageBegin ActivationStage(const std::uint64_t parent) {
            return {.span = IdentityFrom<StreamingTraceSpanId>(301),
                    .parentRoot = IdentityFrom<StreamingTraceOperationId>(101),
                    .parentSpan = IdentityFrom<StreamingTraceSpanId>(parent),
                    .stage = StreamingTraceStage::Activation,
                    .subject = {.cellOperation = Operation(), .activation = IdentityFrom<StreamingCellActivationId>(41)}};
        }

        [[nodiscard]] StreamingTraceStageBegin ProviderStage() {
            return {.span = IdentityFrom<StreamingTraceSpanId>(401),
                    .parentRoot = IdentityFrom<StreamingTraceOperationId>(101),
                    .parentSpan = IdentityFrom<StreamingTraceSpanId>(301),
                    .stage = StreamingTraceStage::ProviderWork,
                    .subject = {.cellOperation = Operation(), .provider = IdentityFrom<StreamingRuntimeServiceId>(51)}};
        }

        [[nodiscard]] WorldStreamingTrace Trace(const std::size_t capacity = 8) {
            auto result = WorldStreamingTrace::Create(Configuration(2, capacity));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    }  // namespace

    TEST_CASE("World Streaming trace publishes typed parented stages", "[unit][world_streaming][trace]") {
        TraceTelemetry telemetry;
        auto trace = Trace(32);
        const auto revision = IdentityFrom<StreamingTraceBindingRevision>(2);
        REQUIRE(trace.Begin(SourceStage(), revision).HasValue());
        std::uint64_t submittedSpan{};
        for (std::uint64_t span = 202; span < 218 && submittedSpan == 0; ++span) {
            REQUIRE(trace.Begin(AssetStage(span), revision).HasValue());
            if (trace.Complete(IdentityFrom<StreamingTraceSpanId>(span), Telemetry::SpanStatus::Succeeded, revision).Value() ==
                StreamingTracePublishDisposition::Submitted)
                submittedSpan = span;
            static_cast<void>(Telemetry::Runtime::Flush(std::chrono::seconds{2}));
        }
        REQUIRE(submittedSpan != 0);
        REQUIRE(trace.Begin(ActivationStage(submittedSpan), revision).HasValue());
        REQUIRE(trace.Begin(ProviderStage(), revision).HasValue());
        REQUIRE(trace.Complete(IdentityFrom<StreamingTraceSpanId>(401), Telemetry::SpanStatus::TimedOut, revision).HasValue());
        REQUIRE(trace.Complete(IdentityFrom<StreamingTraceSpanId>(301), Telemetry::SpanStatus::Cancelled, revision).HasValue());
        REQUIRE(trace.Complete(IdentityFrom<StreamingTraceSpanId>(201), Telemetry::SpanStatus::Succeeded, revision).HasValue());
        REQUIRE(Telemetry::Runtime::Flush(std::chrono::seconds{2}));

        const auto records = telemetry.sink->Records();
        REQUIRE_FALSE(records.empty());
        const auto &asset = std::get<Telemetry::SpanRecord>(records.front().payload);
        CHECK(asset.operationId == submittedSpan);
        CHECK(asset.parentOperationId == 201);
        CHECK(asset.name == "world_streaming.asset_request");
        CHECK(asset.status == Telemetry::SpanStatus::Succeeded);
        CHECK(asset.fields.size() >= 6);
        CHECK(trace.Stages().size() >= 4);
    }

    TEST_CASE("World Streaming trace rejects malformed stale duplicate and over-capacity admission transactionally",
              "[unit][world_streaming][trace][failure]") {
        auto trace = Trace(1);
        const auto revision = IdentityFrom<StreamingTraceBindingRevision>(2);
        auto invalid = SourceStage();
        invalid.subject.assetRequest = IdentityFrom<StreamingCellAssetRequestId>(8);
        RequireError(trace.Begin(invalid, revision), WorldStreamingErrors::TraceInvalid);
        RequireError(trace.Begin(SourceStage(), IdentityFrom<StreamingTraceBindingRevision>(1)), WorldStreamingErrors::TraceStale);
        auto orphan = AssetStage();
        RequireError(trace.Begin(orphan, revision), WorldStreamingErrors::TraceIdentityConflict);
        REQUIRE(trace.Stages().empty());
        REQUIRE(trace.Begin(SourceStage(), revision).HasValue());
        RequireError(trace.Begin(SourceStage(), revision), WorldStreamingErrors::TraceIdentityConflict);
        RequireError(trace.Begin(SourceStage(202), revision), WorldStreamingErrors::TraceCapacityExceeded);
        REQUIRE(trace.Stages().size() == 1);
    }

    TEST_CASE("World Streaming trace rejects malformed configuration and unsupported stages", "[unit][world_streaming][trace][failure]") {
        auto invalid = Configuration();
        invalid.operation = {};
        RequireError(WorldStreamingTrace::Create(invalid), WorldStreamingErrors::TraceInvalid);
        auto excessive = Configuration(2, StreamingTraceConfiguration::MaximumSpanCapacity + 1);
        RequireError(WorldStreamingTrace::Create(excessive), WorldStreamingErrors::TraceCapacityExceeded);

        auto trace = Trace();
        auto unsupported = SourceStage();
        unsupported.stage = StreamingTraceStage::Count;
        RequireError(trace.Begin(unsupported, IdentityFrom<StreamingTraceBindingRevision>(2)), WorldStreamingErrors::TraceUnsupported);
        CHECK(trace.Stages().empty());
    }

    TEST_CASE("World Streaming trace requires terminal once-only completion", "[unit][world_streaming][trace][failure]") {
        auto trace = Trace();
        const auto revision = IdentityFrom<StreamingTraceBindingRevision>(2);
        REQUIRE(trace.Begin(SourceStage(), revision).HasValue());
        RequireError(trace.Complete(IdentityFrom<StreamingTraceSpanId>(201), Telemetry::SpanStatus::Unset, revision),
                     WorldStreamingErrors::TraceUnsupported);
        REQUIRE(trace.Complete(IdentityFrom<StreamingTraceSpanId>(201), Telemetry::SpanStatus::Failed, revision).Value() ==
                StreamingTracePublishDisposition::Dropped);
        RequireError(trace.Complete(IdentityFrom<StreamingTraceSpanId>(201), Telemetry::SpanStatus::Failed, revision),
                     WorldStreamingErrors::TraceLifecycleUnavailable);
        RequireError(trace.Complete(IdentityFrom<StreamingTraceSpanId>(999), Telemetry::SpanStatus::Failed, revision),
                     WorldStreamingErrors::TraceIdentityConflict);
    }

    TEST_CASE("World Streaming trace replacement is revision-fenced and atomic", "[unit][world_streaming][trace][lifecycle]") {
        auto trace = Trace();
        const auto revision = IdentityFrom<StreamingTraceBindingRevision>(2);
        REQUIRE(trace.Begin(SourceStage(), revision).HasValue());
        auto successor = Configuration(3, 4);
        successor.operation = IdentityFrom<StreamingTraceOperationId>(102);
        RequireError(trace.Replace(revision, successor), WorldStreamingErrors::TraceLifecycleUnavailable);
        CHECK(trace.Operation() == IdentityFrom<StreamingTraceOperationId>(101));
        REQUIRE(trace.Complete(IdentityFrom<StreamingTraceSpanId>(201), Telemetry::SpanStatus::Succeeded, revision).HasValue());
        REQUIRE(trace.Replace(revision, successor).HasValue());
        CHECK(trace.Operation() == successor.operation);
        CHECK(trace.Revision() == successor.bindingRevision);
        CHECK(trace.Stages().empty());
        RequireError(trace.Replace(revision, successor), WorldStreamingErrors::TraceStale);

        auto sameRoot = Configuration(4, 4);
        sameRoot.ownerRevision = successor.ownerRevision;
        sameRoot.operation = successor.operation;
        RequireError(trace.Replace(successor.bindingRevision, sameRoot), WorldStreamingErrors::TraceIdentityConflict);
    }

    TEST_CASE("World Streaming trace cancellation and close retain lifecycle evidence", "[unit][world_streaming][trace][lifecycle]") {
        auto trace = Trace();
        const auto revision = IdentityFrom<StreamingTraceBindingRevision>(2);
        REQUIRE(trace.Begin(SourceStage(), revision).HasValue());
        REQUIRE(trace.Begin(AssetStage(), revision).HasValue());
        const auto cancelled = trace.Cancel().Value();
        CHECK(cancelled.submitted == 0);
        CHECK(cancelled.dropped == 2);
        CHECK(trace.Lifecycle() == StreamingTraceLifecycle::Cancelling);
        for (const auto &stage : trace.Stages())
            CHECK(stage.status == Telemetry::SpanStatus::Cancelled);
        RequireError(trace.Begin(SourceStage(300), revision), WorldStreamingErrors::TraceLifecycleUnavailable);
        REQUIRE(trace.Close().HasValue());
        REQUIRE(trace.Close().Value().dropped == 0);
        CHECK(trace.Lifecycle() == StreamingTraceLifecycle::Closed);
    }

    TEST_CASE("World Streaming trace enforces declaring authority thread affinity", "[unit][world_streaming][trace][thread]") {
        auto trace = Trace();
        Result<void> result = Result<void>::Success();
        std::thread foreign([&] {
            result = trace.Begin(SourceStage(), IdentityFrom<StreamingTraceBindingRevision>(2));
        });
        foreign.join();
        RequireError(result, WorldStreamingErrors::TraceThreadAffinityViolation);
        CHECK(trace.Stages().empty());
    }
}  // namespace Horo::WorldStreaming
