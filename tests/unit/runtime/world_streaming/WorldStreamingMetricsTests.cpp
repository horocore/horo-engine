#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "Horo/WorldStreaming/WorldStreamingMetrics.h"
#include "WorldStreamingTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::World;

        struct CapturedMetric {
            Telemetry::Record record;
            Telemetry::InstrumentDescriptor descriptor;
        };

        class MetricCaptureSink final : public Telemetry::ISink {
        public:
            void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *descriptor) override {
                std::lock_guard lock(mutex_);
                captured_.push_back({record, *descriptor});
            }

            void Flush() override {}

            [[nodiscard]] std::vector<CapturedMetric> Snapshot() const {
                std::lock_guard lock(mutex_);
                return captured_;
            }

        private:
            mutable std::mutex mutex_;
            std::vector<CapturedMetric> captured_;
        };

        class MetricsSession final {
        public:
            explicit MetricsSession(const Telemetry::MetricCollectionLevel level, const std::size_t capacity = 1024) {
                static_cast<void>(Telemetry::Runtime::Shutdown());
                sink = std::make_shared<MetricCaptureSink>();
                REQUIRE(Telemetry::Runtime::Initialize({.queueCapacity = capacity, .metricCollectionLevel = level}, sink));
            }

            ~MetricsSession() {
                static_cast<void>(Telemetry::Runtime::Shutdown());
            }

            std::shared_ptr<MetricCaptureSink> sink;
        };

        [[nodiscard]] StreamingRuntimeOwnerToken Owner(const std::uint64_t owner = 7, const std::uint64_t epoch = 3) {
            StreamingRuntimeOwnerToken token;
            token.partition = World();
            token.epoch = IdentityFrom<PartitionEpoch>(epoch);
            token.owner = IdentityFrom<StreamingRuntimeOwnerId>(owner);
            return token;
        }

        [[nodiscard]] StreamingMetricBounds Bounds(const std::uint64_t maximum = 100) {
            return {.maximumLatencySeconds = static_cast<double>(maximum),
                    .maximumBytesPerSample = maximum,
                    .maximumQueueDepth = maximum,
                    .maximumResidentCells = maximum,
                    .maximumDropsPerSample = maximum,
                    .maximumFailuresPerSample = maximum};
        }

        [[nodiscard]] StreamingMetricBindingConfiguration Configuration(const StreamingMetricBindingRevision bindingRevision,
                                                                        const StreamingRuntimeCompositionRevision ownerRevision,
                                                                        const StreamingMetricBounds &bounds,
                                                                        const StreamingMetricAvailability availability,
                                                                        const StreamingMetricRequirement requirement,
                                                                        const Telemetry::MetricCollectionLevel level) {
            return {.bindingRevision = bindingRevision,
                    .ownerRevision = ownerRevision,
                    .bounds = bounds,
                    .availability = availability,
                    .requirement = requirement,
                    .collectionLevel = level};
        }

        [[nodiscard]] StreamingMetricSample Sample(const std::uint64_t ownerRevision = 4) {
            StreamingMetricSample sample{
                .owner = Owner(),
                .ownerRevision = IdentityFrom<StreamingRuntimeCompositionRevision>(ownerRevision),
                .sampleRevision = IdentityFrom<StreamingMetricSampleRevision>(8),
                .diagnosticRevision = IdentityFrom<WorldStreamingDiagnosticRevision>(12),
                .operationLatencySeconds = 0.02,
            };
            sample.stageLatencySeconds.fill(0.003);
            sample.bytes.fill(2);
            sample.queueDepth.fill(3);
            sample.residency.fill(4);
            sample.drops.fill(1);
            sample.failures.fill(1);
            return sample;
        }

        [[nodiscard]] Result<WorldStreamingMetricBinding> AvailableBinding(const Telemetry::MetricCollectionLevel level,
                                                                           const std::uint64_t bindingRevision = 3,
                                                                           const std::uint64_t ownerRevision = 4) {
            return WorldStreamingMetricBinding::Create(Owner(),
                                                       Configuration(IdentityFrom<StreamingMetricBindingRevision>(bindingRevision),
                                                                     IdentityFrom<StreamingRuntimeCompositionRevision>(ownerRevision),
                                                                     Bounds(), StreamingMetricAvailability::Available,
                                                                     StreamingMetricRequirement::Required, level),
                                                       RegisterStreamingMetricHandles(level));
        }

        [[nodiscard]] WorldStreamingMetricBinding OwnedAvailableBinding(const Telemetry::MetricCollectionLevel level) {
            auto result = AvailableBinding(level);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        void PublishSampleSequence(WorldStreamingMetricBinding &binding) {
            for (std::uint64_t revision = 8; revision < 40; ++revision) {
                auto sample = Sample();
                sample.sampleRevision = IdentityFrom<StreamingMetricSampleRevision>(revision);
                REQUIRE(binding.Publish(sample, IdentityFrom<StreamingMetricBindingRevision>(3)).Value() ==
                        StreamingMetricPublishDisposition::Submitted);
            }
        }

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE_FALSE(result.HasValue());
            const std::string_view actualCode = result.ErrorValue().code.Value();
            CHECK(actualCode == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("World Streaming metric binding preserves explicit policy and capability states", "[unit][world_streaming][metrics]") {
        const auto revision = IdentityFrom<StreamingMetricBindingRevision>(3);
        const auto ownerRevision = IdentityFrom<StreamingRuntimeCompositionRevision>(4);
        auto off =
            WorldStreamingMetricBinding::Create(Owner(),
                                                Configuration(revision, ownerRevision, Bounds(), StreamingMetricAvailability::Off,
                                                              StreamingMetricRequirement::Optional, Telemetry::MetricCollectionLevel::Off),
                                                {});
        REQUIRE(off.HasValue());
        REQUIRE(off.Value().Owner() == Owner());
        REQUIRE(off.Value().Revision() == revision);
        REQUIRE(off.Value().OwnerRevision() == ownerRevision);
        REQUIRE(off.Value().State() == StreamingMetricBindingState::Active);
        REQUIRE(off.Value().Publish(Sample(), revision).Value() == StreamingMetricPublishDisposition::SuppressedByPolicy);
        RequireError(off.Value().Publish(Sample(), revision), WorldStreamingErrors::MetricStale);

        auto unavailable =
            WorldStreamingMetricBinding::Create(Owner(),
                                                Configuration(revision, ownerRevision, Bounds(), StreamingMetricAvailability::Unavailable,
                                                              StreamingMetricRequirement::Optional, Telemetry::MetricCollectionLevel::Core),
                                                {});
        REQUIRE(unavailable.HasValue());
        REQUIRE(unavailable.Value().Publish(Sample(), revision).Value() == StreamingMetricPublishDisposition::SuppressedUnavailable);
    }

    TEST_CASE("World Streaming metric binding rejects inconsistent or unsupported configuration",
              "[unit][world_streaming][metrics][failure]") {
        const auto revision = IdentityFrom<StreamingMetricBindingRevision>(3);
        const auto ownerRevision = IdentityFrom<StreamingRuntimeCompositionRevision>(4);
        RequireError(WorldStreamingMetricBinding::Create(Owner(),
                                                         Configuration(revision, ownerRevision, Bounds(),
                                                                       StreamingMetricAvailability::Unavailable,
                                                                       StreamingMetricRequirement::Required,
                                                                       Telemetry::MetricCollectionLevel::Core),
                                                         {}),
                     WorldStreamingErrors::MetricCapabilityUnavailable);
        RequireError(WorldStreamingMetricBinding::Create(Owner(),
                                                         Configuration(revision, ownerRevision, Bounds(), StreamingMetricAvailability::Off,
                                                                       StreamingMetricRequirement::Optional,
                                                                       Telemetry::MetricCollectionLevel::Core),
                                                         {}),
                     WorldStreamingErrors::MetricInvalid);
        RequireError(WorldStreamingMetricBinding::Create(Owner(),
                                                         Configuration(revision, ownerRevision, Bounds(),
                                                                       static_cast<StreamingMetricAvailability>(255),
                                                                       StreamingMetricRequirement::Optional,
                                                                       Telemetry::MetricCollectionLevel::Core),
                                                         {}),
                     WorldStreamingErrors::MetricUnsupported);
        auto invalidBounds = Bounds();
        invalidBounds.maximumQueueDepth = 0;
        RequireError(WorldStreamingMetricBinding::Create(Owner(),
                                                         Configuration(revision, ownerRevision, invalidBounds,
                                                                       StreamingMetricAvailability::Off,
                                                                       StreamingMetricRequirement::Optional,
                                                                       Telemetry::MetricCollectionLevel::Off),
                                                         {}),
                     WorldStreamingErrors::MetricInvalid);
    }

    TEST_CASE("World Streaming metric samples reject malformed stale and over-capacity values before emission",
              "[unit][world_streaming][metrics][failure]") {
        MetricsSession telemetry{Telemetry::MetricCollectionLevel::Detailed};
        auto binding = OwnedAvailableBinding(Telemetry::MetricCollectionLevel::Detailed);
        const auto revision = IdentityFrom<StreamingMetricBindingRevision>(3);
        const auto accepted = Telemetry::Runtime::GetStatistics().acceptedRecords;

        auto sample = Sample();
        sample.schemaVersion = 2;
        RequireError(binding.Publish(sample, revision), WorldStreamingErrors::MetricInvalid);
        sample = Sample();
        sample.operationLatencySeconds = std::numeric_limits<double>::quiet_NaN();
        RequireError(binding.Publish(sample, revision), WorldStreamingErrors::MetricInvalid);
        sample = Sample(5);
        RequireError(binding.Publish(sample, revision), WorldStreamingErrors::MetricStale);
        RequireError(binding.Publish(Sample(), IdentityFrom<StreamingMetricBindingRevision>(2)), WorldStreamingErrors::MetricStale);

        sample = Sample();
        sample.stageLatencySeconds[0] = 101.0;
        RequireError(binding.Publish(sample, revision), WorldStreamingErrors::MetricCapacityExceeded);
        sample = Sample();
        sample.bytes[0] = 101;
        RequireError(binding.Publish(sample, revision), WorldStreamingErrors::MetricCapacityExceeded);
        sample = Sample();
        sample.queueDepth[0] = 101;
        RequireError(binding.Publish(sample, revision), WorldStreamingErrors::MetricCapacityExceeded);
        sample = Sample();
        sample.residency = {25, 25, 25, 26};
        RequireError(binding.Publish(sample, revision), WorldStreamingErrors::MetricCapacityExceeded);
        sample = Sample();
        sample.drops[0] = 101;
        RequireError(binding.Publish(sample, revision), WorldStreamingErrors::MetricCapacityExceeded);
        sample = Sample();
        sample.failures[0] = 101;
        RequireError(binding.Publish(sample, revision), WorldStreamingErrors::MetricCapacityExceeded);
        REQUIRE(Telemetry::Runtime::GetStatistics().acceptedRecords == accepted);
    }

    TEST_CASE("World Streaming publishes the complete bounded metric family without identity dimensions",
              "[unit][world_streaming][metrics][cardinality]") {
        MetricsSession telemetry{Telemetry::MetricCollectionLevel::Detailed};
        auto binding = OwnedAvailableBinding(Telemetry::MetricCollectionLevel::Detailed);
        const auto before = Telemetry::Runtime::GetStatistics();
        PublishSampleSequence(binding);
        REQUIRE(Telemetry::Runtime::Flush(std::chrono::seconds{2}));

        const auto captured = telemetry.sink->Snapshot();
        std::set<std::tuple<std::string, std::uint16_t>> series;
        for (const CapturedMetric &entry : captured) {
            const auto &descriptor = entry.descriptor;
            REQUIRE(descriptor.subsystem == "world_streaming");
            REQUIRE(descriptor.dimensions.size() <= 1);
            REQUIRE(descriptor.maxSeries <= StreamingMetricFailureReasonCount);
            if (!descriptor.dimensions.empty()) {
                const auto &key = descriptor.dimensions.front().key;
                REQUIRE((key == "stage" || key == "flow" || key == "queue" || key == "state" || key == "reason"));
            }
            const auto &metric = std::get<Telemetry::MetricRecord>(entry.record.payload);
            series.emplace(descriptor.name, metric.dimensionCount == 0 ? 0 : metric.dimensionValueIds[0]);
        }
        REQUIRE(series.size() == 22);
        const auto after = Telemetry::Runtime::GetStatistics();
        REQUIRE(after.acceptedRecords - before.acceptedRecords + after.droppedRecords - before.droppedRecords == 32 * 22);
    }

    TEST_CASE("Core collection omits detailed stages while retaining all aggregate streaming health", "[unit][world_streaming][metrics]") {
        MetricsSession telemetry{Telemetry::MetricCollectionLevel::Core};
        auto binding = OwnedAvailableBinding(Telemetry::MetricCollectionLevel::Core);
        const auto before = Telemetry::Runtime::GetStatistics();
        PublishSampleSequence(binding);
        REQUIRE(Telemetry::Runtime::Flush(std::chrono::seconds{2}));
        const auto after = Telemetry::Runtime::GetStatistics();
        REQUIRE(after.acceptedRecords - before.acceptedRecords + after.droppedRecords - before.droppedRecords == 32 * 17);
        std::set<std::tuple<std::string, std::uint16_t>> series;
        const auto captured = telemetry.sink->Snapshot();
        for (const CapturedMetric &entry : captured) {
            const auto &metric = std::get<Telemetry::MetricRecord>(entry.record.payload);
            series.emplace(entry.descriptor.name, metric.dimensionCount == 0 ? 0 : metric.dimensionValueIds[0]);
        }
        REQUIRE(series.size() == 17);
        for (const CapturedMetric &entry : captured)
            REQUIRE(entry.descriptor.name != "horo.world_streaming.stage.duration");
    }

    TEST_CASE("Metric binding replacement is revision fenced and transactional", "[unit][world_streaming][metrics][replacement]") {
        MetricsSession telemetry{Telemetry::MetricCollectionLevel::Core};
        auto binding = OwnedAvailableBinding(Telemetry::MetricCollectionLevel::Core);
        const auto current = IdentityFrom<StreamingMetricBindingRevision>(3);
        const auto successor = IdentityFrom<StreamingMetricBindingRevision>(5);

        auto invalidBounds = Bounds();
        invalidBounds.maximumBytesPerSample = 0;
        RequireError(binding.Replace(current,
                                     Configuration(successor, IdentityFrom<StreamingRuntimeCompositionRevision>(6), invalidBounds,
                                                   StreamingMetricAvailability::Off, StreamingMetricRequirement::Optional,
                                                   Telemetry::MetricCollectionLevel::Off),
                                     {}),
                     WorldStreamingErrors::MetricInvalid);
        REQUIRE(binding.Revision() == current);
        REQUIRE(binding.OwnerRevision() == IdentityFrom<StreamingRuntimeCompositionRevision>(4));

        REQUIRE(binding
                    .Replace(current,
                             Configuration(successor, IdentityFrom<StreamingRuntimeCompositionRevision>(6), Bounds(),
                                           StreamingMetricAvailability::Off, StreamingMetricRequirement::Optional,
                                           Telemetry::MetricCollectionLevel::Off),
                             {})
                    .HasValue());
        REQUIRE(binding.Revision() == successor);
        RequireError(binding.Publish(Sample(), successor), WorldStreamingErrors::MetricStale);
        REQUIRE(binding.Publish(Sample(6), successor).Value() == StreamingMetricPublishDisposition::SuppressedByPolicy);
        RequireError(binding.Replace(current,
                                     Configuration(IdentityFrom<StreamingMetricBindingRevision>(7),
                                                   IdentityFrom<StreamingRuntimeCompositionRevision>(6), Bounds(),
                                                   StreamingMetricAvailability::Off, StreamingMetricRequirement::Optional,
                                                   Telemetry::MetricCollectionLevel::Off),
                                     {}),
                     WorldStreamingErrors::MetricStale);
    }

    TEST_CASE("Metric binding cancellation shutdown move and thread affinity preserve unique ownership",
              "[unit][world_streaming][metrics][lifecycle]") {
        MetricsSession telemetry{Telemetry::MetricCollectionLevel::Core};
        auto binding = OwnedAvailableBinding(Telemetry::MetricCollectionLevel::Core);
        bool foreignRejected{};
        std::thread foreign([&binding, &foreignRejected] {
            const auto result = binding.Publish(Sample(), IdentityFrom<StreamingMetricBindingRevision>(3));
            foreignRejected =
                result.HasError() && result.ErrorValue().code.Value() == WorldStreamingErrors::MetricThreadAffinityViolation.code.Value();
        });
        foreign.join();
        REQUIRE(foreignRejected);

        auto moved = std::move(binding);
        REQUIRE(binding.State() == StreamingMetricBindingState::Closed);
        REQUIRE(moved.Cancel().HasValue());
        REQUIRE(moved.Cancel().HasValue());
        REQUIRE(moved.State() == StreamingMetricBindingState::Cancelled);
        RequireError(moved.Publish(Sample(), IdentityFrom<StreamingMetricBindingRevision>(3)),
                     WorldStreamingErrors::MetricLifecycleUnavailable);
        RequireError(moved.Replace(IdentityFrom<StreamingMetricBindingRevision>(3),
                                   Configuration(IdentityFrom<StreamingMetricBindingRevision>(4),
                                                 IdentityFrom<StreamingRuntimeCompositionRevision>(4), Bounds(),
                                                 StreamingMetricAvailability::Available, StreamingMetricRequirement::Required,
                                                 Telemetry::MetricCollectionLevel::Core),
                                   RegisterStreamingMetricHandles(Telemetry::MetricCollectionLevel::Core)),
                     WorldStreamingErrors::MetricLifecycleUnavailable);
        REQUIRE(moved.Close().HasValue());
        REQUIRE(moved.Close().HasValue());
        REQUIRE(moved.State() == StreamingMetricBindingState::Closed);
    }
}  // namespace Horo::WorldStreaming
