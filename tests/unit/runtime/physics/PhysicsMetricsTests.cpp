#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsMetrics.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Physics {
    namespace {
        class CollectingSink final : public Telemetry::ISink {
        public:
            void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *descriptor) override {
                std::lock_guard lock(mutex_);
                records_.push_back(record);
                descriptors_.push_back(*descriptor);
            }

            void Flush() override {}

            [[nodiscard]] std::vector<Telemetry::Record> Records() const {
                std::lock_guard lock(mutex_);
                return records_;
            }

            [[nodiscard]] std::vector<Telemetry::InstrumentDescriptor> Descriptors() const {
                std::lock_guard lock(mutex_);
                return descriptors_;
            }

        private:
            mutable std::mutex mutex_;
            std::vector<Telemetry::Record> records_;
            std::vector<Telemetry::InstrumentDescriptor> descriptors_;
        };

        class TelemetryGuard final {
        public:
            explicit TelemetryGuard(const Telemetry::MetricCollectionLevel level, const std::size_t capacity = 512)
                : sink(std::make_shared<CollectingSink>()) {
                static_cast<void>(Telemetry::Runtime::Shutdown());
                REQUIRE(Telemetry::Runtime::Initialize({.queueCapacity = capacity, .metricCollectionLevel = level}, sink));
            }

            ~TelemetryGuard() {
                static_cast<void>(Telemetry::Runtime::Shutdown());
            }

            std::shared_ptr<CollectingSink> sink;
        };

        [[nodiscard]] PhysicsWorldId World(const std::uint64_t value = 91) {
            const auto world = PhysicsWorldId::Create(value);
            REQUIRE(world.HasValue());
            return world.Value();
        }

        [[nodiscard]] PhysicsMetricBounds Bounds(const std::uint64_t maximum = 10) {
            return {.maximumBodies = maximum,
                    .maximumShapes = maximum,
                    .maximumConstraints = maximum,
                    .maximumBroadphasePairs = maximum,
                    .maximumContacts = maximum,
                    .maximumQueriesPerTick = maximum,
                    .maximumCommandDepth = maximum,
                    .maximumEventDepth = maximum};
        }

        [[nodiscard]] PhysicsMetricSnapshot Snapshot(const std::uint64_t maximum = 10) {
            return {.world = World(),
                    .publicationRevision = 4,
                    .simulationTick = 8,
                    .fixedStepSeconds = 0.004,
                    .broadphaseSeconds = 0.001,
                    .narrowphaseSeconds = 0.0015,
                    .solverSeconds = 0.001,
                    .bodyCount = maximum,
                    .sleepingBodyCount = maximum,
                    .shapeCount = maximum,
                    .constraintCount = maximum,
                    .broadphasePairCount = maximum,
                    .contactCount = maximum,
                    .queryCount = maximum,
                    .commandDepth = maximum,
                    .eventDepth = maximum,
                    .droppedEventCount = maximum,
                    .overflowCount = 2};
        }

        [[nodiscard]] Result<PhysicsMetricBinding> AvailableBinding(const Telemetry::MetricCollectionLevel level) {
            return PhysicsMetricBinding::Create(World(), 3, Bounds(), PhysicsMetricAvailability::Available,
                                                PhysicsMetricRequirement::Required, level, RegisterPhysicsMetricHandles(level));
        }

        void RequireError(const Result<PhysicsMetricPublishDisposition> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Physics metric binding preserves explicit off unavailable and required states", "[physics][metrics]") {
        const auto off = PhysicsMetricBinding::Create(World(), 3, Bounds(), PhysicsMetricAvailability::Off,
                                                      PhysicsMetricRequirement::Optional, Telemetry::MetricCollectionLevel::Off, {});
        REQUIRE(off.HasValue());
        REQUIRE(off.Value().World() == World());
        REQUIRE(off.Value().Revision() == 3);
        REQUIRE(off.Value().Availability() == PhysicsMetricAvailability::Off);
        REQUIRE(off.Value().Publish(Snapshot(), 3, 4).Value() == PhysicsMetricPublishDisposition::SuppressedByPolicy);

        const auto unavailable =
            PhysicsMetricBinding::Create(World(), 3, Bounds(), PhysicsMetricAvailability::Unavailable, PhysicsMetricRequirement::Optional,
                                         Telemetry::MetricCollectionLevel::Core, {});
        REQUIRE(unavailable.HasValue());
        REQUIRE(unavailable.Value().Publish(Snapshot(), 3, 4).Value() == PhysicsMetricPublishDisposition::SuppressedUnavailable);

        const auto required = PhysicsMetricBinding::Create(World(), 3, Bounds(), PhysicsMetricAvailability::Unavailable,
                                                           PhysicsMetricRequirement::Required, Telemetry::MetricCollectionLevel::Core, {});
        REQUIRE(required.HasError());
        REQUIRE(required.ErrorValue().code.Value() == PhysicsErrors::CapabilityUnavailable.code.Value());

        REQUIRE(PhysicsMetricBinding::Create(World(), 3, Bounds(), PhysicsMetricAvailability::Off, PhysicsMetricRequirement::Optional,
                                             Telemetry::MetricCollectionLevel::Core, {})
                    .HasError());
        REQUIRE(PhysicsMetricBinding::Create({}, 3, Bounds(), PhysicsMetricAvailability::Off, PhysicsMetricRequirement::Optional,
                                             Telemetry::MetricCollectionLevel::Off, {})
                    .HasError());
        auto zeroBounds = Bounds();
        zeroBounds.maximumContacts = 0;
        REQUIRE(PhysicsMetricBinding::Create(World(), 3, zeroBounds, PhysicsMetricAvailability::Off, PhysicsMetricRequirement::Optional,
                                             Telemetry::MetricCollectionLevel::Off, {})
                    .HasError());
    }

    TEST_CASE("Physics metric snapshots reject stale malformed and one-over-bound values before emission", "[physics][metrics]") {
        TelemetryGuard telemetry{Telemetry::MetricCollectionLevel::Detailed};
        auto binding = AvailableBinding(Telemetry::MetricCollectionLevel::Detailed);
        REQUIRE(binding.HasValue());
        auto snapshot = Snapshot();

        const auto accepted = Telemetry::Runtime::GetStatistics().acceptedRecords;
        RequireError(binding.Value().Publish(snapshot, 2, 4), PhysicsErrors::CapabilityStale);
        RequireError(binding.Value().Publish(snapshot, 3, 3), PhysicsErrors::QuerySnapshotStale);
        snapshot.world = World(92);
        RequireError(binding.Value().Publish(snapshot, 3, 4), PhysicsErrors::DescriptorInvalid);
        const std::array durationMembers{&PhysicsMetricSnapshot::fixedStepSeconds, &PhysicsMetricSnapshot::broadphaseSeconds,
                                         &PhysicsMetricSnapshot::narrowphaseSeconds, &PhysicsMetricSnapshot::solverSeconds};
        for (const auto member : durationMembers) {
            snapshot = Snapshot();
            snapshot.*member = -0.01;
            RequireError(binding.Value().Publish(snapshot, 3, 4), PhysicsErrors::DescriptorInvalid);
            snapshot.*member = std::numeric_limits<double>::quiet_NaN();
            RequireError(binding.Value().Publish(snapshot, 3, 4), PhysicsErrors::DescriptorInvalid);
        }
        const std::array countMembers{&PhysicsMetricSnapshot::bodyCount,        &PhysicsMetricSnapshot::shapeCount,
                                      &PhysicsMetricSnapshot::constraintCount,  &PhysicsMetricSnapshot::broadphasePairCount,
                                      &PhysicsMetricSnapshot::contactCount,     &PhysicsMetricSnapshot::queryCount,
                                      &PhysicsMetricSnapshot::commandDepth,     &PhysicsMetricSnapshot::eventDepth,
                                      &PhysicsMetricSnapshot::droppedEventCount};
        for (const auto member : countMembers) {
            snapshot = Snapshot();
            snapshot.*member = 11;
            RequireError(binding.Value().Publish(snapshot, 3, 4), PhysicsErrors::DescriptorInvalid);
        }
        snapshot = Snapshot();
        snapshot.sleepingBodyCount = snapshot.bodyCount + 1;
        RequireError(binding.Value().Publish(snapshot, 3, 4), PhysicsErrors::DescriptorInvalid);
        snapshot = Snapshot();
        snapshot.overflowCount = 3;
        RequireError(binding.Value().Publish(snapshot, 3, 4), PhysicsErrors::DescriptorInvalid);
        REQUIRE(Telemetry::Runtime::GetStatistics().acceptedRecords == accepted);
    }

    TEST_CASE("Physics publishes the complete closed metric family with stable kinds units and dimensions", "[physics][metrics]") {
        TelemetryGuard telemetry{Telemetry::MetricCollectionLevel::Detailed};
        auto binding = AvailableBinding(Telemetry::MetricCollectionLevel::Detailed);
        REQUIRE(binding.HasValue());
        const auto before = Telemetry::Runtime::GetStatistics();
        for (std::uint64_t revision = 4; revision < 36; ++revision) {
            auto snapshot = Snapshot();
            snapshot.publicationRevision = revision;
            snapshot.simulationTick = revision;
            const auto result = binding.Value().Publish(snapshot, 3, revision);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value() == PhysicsMetricPublishDisposition::Submitted);
        }
        REQUIRE(Telemetry::Runtime::Flush(std::chrono::seconds{2}));

        const auto records = telemetry.sink->Records();
        const auto descriptors = telemetry.sink->Descriptors();
        REQUIRE(descriptors.size() == records.size());
        std::set<std::pair<std::string, std::uint16_t>> observedSeries;
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
            const auto &descriptor = descriptors[index];
            REQUIRE(descriptor.subsystem == "physics");
            if (descriptor.name.ends_with("duration")) {
                REQUIRE(descriptor.kind == Telemetry::InstrumentKind::Histogram);
                REQUIRE(descriptor.unit == Telemetry::MetricUnit::Seconds);
            } else if (descriptor.kind == Telemetry::InstrumentKind::Gauge) {
                REQUIRE(descriptor.unit == Telemetry::MetricUnit::Count);
            } else {
                REQUIRE(descriptor.kind == Telemetry::InstrumentKind::Counter);
                REQUIRE(descriptor.unit == Telemetry::MetricUnit::Count);
            }
            const auto &metric = std::get<Telemetry::MetricRecord>(records[index].payload);
            observedSeries.emplace(descriptor.name, metric.dimensionCount == 0 ? 0 : metric.dimensionValueIds[0]);
        }
        REQUIRE(observedSeries.size() == 15);
        const auto after = Telemetry::Runtime::GetStatistics();
        REQUIRE(after.acceptedRecords - before.acceptedRecords + after.droppedRecords - before.droppedRecords == 32 * 15);
    }

    TEST_CASE("Core collection omits detailed stages while retaining bounded world measurements", "[physics][metrics]") {
        TelemetryGuard telemetry{Telemetry::MetricCollectionLevel::Core};
        auto binding = AvailableBinding(Telemetry::MetricCollectionLevel::Core);
        REQUIRE(binding.HasValue());
        const auto before = Telemetry::Runtime::GetStatistics();
        REQUIRE(binding.Value().Publish(Snapshot(), 3, 4).HasValue());
        REQUIRE(Telemetry::Runtime::Flush(std::chrono::seconds{2}));
        const auto after = Telemetry::Runtime::GetStatistics();
        REQUIRE(after.acceptedRecords - before.acceptedRecords + after.droppedRecords - before.droppedRecords == 12);
        for (const auto &descriptor : telemetry.sink->Descriptors())
            REQUIRE(descriptor.name != "horo.physics.stage.duration");
    }

    TEST_CASE("Physics metric binding enforces owner thread and closes idempotently before world retirement", "[physics][metrics]") {
        TelemetryGuard telemetry{Telemetry::MetricCollectionLevel::Core, 1024};
        auto binding = AvailableBinding(Telemetry::MetricCollectionLevel::Core);
        REQUIRE(binding.HasValue());
        auto ownedBinding = std::move(binding).Value();
        const auto snapshot = Snapshot();
        const auto before = Telemetry::Runtime::GetStatistics();
        bool foreignRejected{};
        std::thread foreign([&ownedBinding, &foreignRejected, snapshot] {
            const auto result = ownedBinding.Publish(snapshot, 3, 4);
            foreignRejected = result.HasError() && result.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
        });
        foreign.join();
        REQUIRE(foreignRejected);
        const auto after = Telemetry::Runtime::GetStatistics();
        REQUIRE(after.acceptedRecords == before.acceptedRecords);
        REQUIRE(after.droppedRecords == before.droppedRecords);
        REQUIRE(ownedBinding.Close().HasValue());
        REQUIRE(ownedBinding.Close().HasValue());
        RequireError(ownedBinding.Publish(Snapshot(), 3, 4), PhysicsErrors::InvalidState);
    }

    TEST_CASE("Stopped Telemetry handles become safe no-ops without becoming Physics control flow", "[physics][metrics]") {
        TelemetryGuard telemetry{Telemetry::MetricCollectionLevel::Core};
        auto binding = AvailableBinding(Telemetry::MetricCollectionLevel::Core);
        REQUIRE(binding.HasValue());
        static_cast<void>(Telemetry::Runtime::Shutdown());
        const auto before = Telemetry::Runtime::GetStatistics();
        const auto result = binding.Value().Publish(Snapshot(), 3, 4);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value() == PhysicsMetricPublishDisposition::Submitted);
        const auto after = Telemetry::Runtime::GetStatistics();
        REQUIRE(after.staleHandleDrops == before.staleHandleDrops);
        REQUIRE(after.acceptedRecords == before.acceptedRecords);
        REQUIRE(after.droppedRecords == before.droppedRecords);
    }
}  // namespace Horo::Physics
