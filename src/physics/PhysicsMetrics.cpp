#include "Horo/Physics/PhysicsMetrics.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Physics {
    namespace {
        constexpr std::array<std::string_view, 3> kStageValues{"broadphase", "narrowphase", "solver"};
        constexpr std::array<std::string_view, 7> kCountValues{"bodies",           "sleeping_bodies", "shapes", "constraints",
                                                               "broadphase_pairs", "contacts",        "queries"};
        constexpr std::array<std::string_view, 2> kDepthValues{"commands", "events"};
        constexpr std::array<std::string_view, 2> kEventValues{"dropped_events", "overflow"};
        constexpr std::size_t kBroadphaseStageIndex = 0;
        constexpr std::size_t kNarrowphaseStageIndex = 1;
        constexpr std::size_t kSolverStageIndex = 2;
        constexpr std::size_t kCommandDepthIndex = 0;
        constexpr std::size_t kEventDepthIndex = 1;
        constexpr std::size_t kDroppedEventIndex = 0;
        constexpr std::size_t kOverflowEventIndex = 1;

        [[nodiscard]] Telemetry::DimensionDescriptor Dimension(std::string key, const auto &values) {
            Telemetry::DimensionDescriptor result{.key = std::move(key)};
            result.allowedValues.reserve(values.size());
            for (const std::string_view value : values)
                result.allowedValues.emplace_back(value);
            return result;
        }

        [[nodiscard]] Telemetry::InstrumentDescriptor Descriptor(Telemetry::InstrumentKind kind, std::string name,
                                                                 const Telemetry::MetricUnit unit, std::string description,
                                                                 Telemetry::MetricCollectionLevel level,
                                                                 std::vector<Telemetry::DimensionDescriptor> dimensions = {}) {
            const auto maximumSeries = dimensions.empty() ? 1U : static_cast<std::uint32_t>(dimensions.front().allowedValues.size());
            return {.kind = kind,
                    .name = std::move(name),
                    .subsystem = "physics",
                    .unit = unit,
                    .description = std::move(description),
                    .dimensions = std::move(dimensions),
                    .maxSeries = maximumSeries,
                    .minimumCollectionLevel = level};
        }

        template <typename Handle, std::size_t Size>
        [[nodiscard]] bool AllHandlesAvailable(const std::array<Handle, Size> &handles) noexcept {
            return std::ranges::all_of(handles, [](const Handle &handle) {
                return static_cast<bool>(handle);
            });
        }

        [[nodiscard]] bool CoreHandlesAvailable(const PhysicsMetricHandles &handles) noexcept {
            return handles.fixedStepDuration && AllHandlesAvailable(handles.counts) && AllHandlesAvailable(handles.depths) &&
                   AllHandlesAvailable(handles.events);
        }

        [[nodiscard]] bool BoundsValid(const PhysicsMetricBounds &bounds) noexcept {
            return bounds.maximumBodies != 0 && bounds.maximumShapes != 0 && bounds.maximumConstraints != 0 &&
                   bounds.maximumBroadphasePairs != 0 && bounds.maximumContacts != 0 && bounds.maximumQueriesPerTick != 0 &&
                   bounds.maximumCommandDepth != 0 && bounds.maximumEventDepth != 0;
        }

        [[nodiscard]] bool DurationValid(const double value) noexcept {
            return std::isfinite(value) && value >= 0.0;
        }

        [[nodiscard]] bool SnapshotIdentityValid(const PhysicsMetricSnapshot &snapshot, const PhysicsWorldId world) noexcept {
            return snapshot.schemaVersion == 1 && snapshot.world == world && snapshot.publicationRevision != 0 &&
                   snapshot.simulationTick != 0;
        }

        [[nodiscard]] bool SnapshotDurationsValid(const PhysicsMetricSnapshot &snapshot) noexcept {
            return DurationValid(snapshot.fixedStepSeconds) && DurationValid(snapshot.broadphaseSeconds) &&
                   DurationValid(snapshot.narrowphaseSeconds) && DurationValid(snapshot.solverSeconds);
        }

        [[nodiscard]] bool SnapshotCountsValid(const PhysicsMetricSnapshot &snapshot, const PhysicsMetricBounds &bounds) noexcept {
            return snapshot.bodyCount <= bounds.maximumBodies && snapshot.sleepingBodyCount <= snapshot.bodyCount &&
                   snapshot.shapeCount <= bounds.maximumShapes && snapshot.constraintCount <= bounds.maximumConstraints &&
                   snapshot.broadphasePairCount <= bounds.maximumBroadphasePairs && snapshot.contactCount <= bounds.maximumContacts &&
                   snapshot.queryCount <= bounds.maximumQueriesPerTick && snapshot.commandDepth <= bounds.maximumCommandDepth &&
                   snapshot.eventDepth <= bounds.maximumEventDepth && snapshot.droppedEventCount <= bounds.maximumEventDepth &&
                   snapshot.overflowCount <= 2;
        }

        [[nodiscard]] bool SnapshotValid(const PhysicsMetricSnapshot &snapshot, const PhysicsWorldId world,
                                         const PhysicsMetricBounds &bounds) noexcept {
            return SnapshotIdentityValid(snapshot, world) && SnapshotDurationsValid(snapshot) && SnapshotCountsValid(snapshot, bounds);
        }

        template <typename Handle, std::size_t Size>
        void BindHandles(std::array<Handle, Size> &output, const Handle &root, const std::string_view dimension,
                         const std::array<std::string_view, Size> &values) {
            for (std::size_t index = 0; index < Size; ++index) {
                const std::array selection{Telemetry::DimensionValue{dimension, values[index]}};
                output[index] = root.WithDimensions(selection);
            }
        }
    }  // namespace

    /** @copydoc RegisterPhysicsMetricHandles */
    PhysicsMetricHandles RegisterPhysicsMetricHandles(const Telemetry::MetricCollectionLevel level) {
        using enum Telemetry::MetricCollectionLevel;
        PhysicsMetricHandles handles;
        if (level == Off || level > Detailed)
            return handles;

        handles.fixedStepDuration = Telemetry::Runtime::RegisterHistogram(
            Descriptor(Telemetry::InstrumentKind::Histogram, "horo.physics.fixed_step.duration", Telemetry::MetricUnit::Seconds,
                       "Host-measured duration of one committed Physics fixed step.", Core));

        auto countRoot = Telemetry::Runtime::RegisterGauge(Descriptor(Telemetry::InstrumentKind::Gauge, "horo.physics.count",
                                                                      Telemetry::MetricUnit::Count, "Current bounded Physics item count.",
                                                                      Core, {Dimension("kind", kCountValues)}));
        BindHandles(handles.counts, countRoot, "kind", kCountValues);

        auto depthRoot = Telemetry::Runtime::RegisterGauge(Descriptor(Telemetry::InstrumentKind::Gauge, "horo.physics.queue.depth",
                                                                      Telemetry::MetricUnit::Count, "Current bounded Physics queue depth.",
                                                                      Core, {Dimension("kind", kDepthValues)}));
        BindHandles(handles.depths, depthRoot, "kind", kDepthValues);

        auto eventRoot = Telemetry::Runtime::RegisterCounter(
            Descriptor(Telemetry::InstrumentKind::Counter, "horo.physics.event", Telemetry::MetricUnit::Count,
                       "Physics observation loss and overflow events.", Core, {Dimension("kind", kEventValues)}));
        BindHandles(handles.events, eventRoot, "kind", kEventValues);

        if (level == Detailed) {
            auto stageRoot = Telemetry::Runtime::RegisterHistogram(
                Descriptor(Telemetry::InstrumentKind::Histogram, "horo.physics.stage.duration", Telemetry::MetricUnit::Seconds,
                           "Host or adapter measured Physics pipeline stage duration.", Detailed, {Dimension("stage", kStageValues)}));
            BindHandles(handles.stageDurations, stageRoot, "stage", kStageValues);
        }
        return handles;
    }

    /** @copydoc PhysicsMetricBinding::Create */
    Result<PhysicsMetricBinding> PhysicsMetricBinding::Create(const PhysicsWorldId world, const std::uint64_t revision,
                                                              const PhysicsMetricBounds &bounds,
                                                              const PhysicsMetricAvailability availability,
                                                              const PhysicsMetricRequirement requirement,
                                                              const Telemetry::MetricCollectionLevel level, PhysicsMetricHandles handles) {
        if (const bool enumValid = availability <= PhysicsMetricAvailability::Available &&
                                   requirement <= PhysicsMetricRequirement::Required && level <= Telemetry::MetricCollectionLevel::Detailed;
            !world.IsValid() || revision == 0 || !BoundsValid(bounds) || !enumValid)
            return Result<PhysicsMetricBinding>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (requirement == PhysicsMetricRequirement::Required && availability != PhysicsMetricAvailability::Available)
            return Result<PhysicsMetricBinding>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if ((availability == PhysicsMetricAvailability::Off) != (level == Telemetry::MetricCollectionLevel::Off))
            return Result<PhysicsMetricBinding>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (availability == PhysicsMetricAvailability::Available) {
            const bool detailedAvailable =
                level != Telemetry::MetricCollectionLevel::Detailed || AllHandlesAvailable(handles.stageDurations);
            if (!CoreHandlesAvailable(handles) || !detailedAvailable)
                return Result<PhysicsMetricBinding>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        }
        return Result<PhysicsMetricBinding>::Success(
            PhysicsMetricBinding{world, revision, bounds, availability, level, std::move(handles)});
    }

    /** @copydoc PhysicsMetricBinding::Publish */
    Result<PhysicsMetricPublishDisposition> PhysicsMetricBinding::Publish(const PhysicsMetricSnapshot &snapshot,
                                                                          const std::uint64_t expectedBindingRevision,
                                                                          const std::uint64_t expectedPublicationRevision) const {
        if (std::this_thread::get_id() != ownerThread_)
            return Result<PhysicsMetricPublishDisposition>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (closed_)
            return Result<PhysicsMetricPublishDisposition>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (expectedBindingRevision == 0 || expectedBindingRevision != revision_)
            return Result<PhysicsMetricPublishDisposition>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        if (expectedPublicationRevision == 0 || snapshot.publicationRevision != expectedPublicationRevision)
            return Result<PhysicsMetricPublishDisposition>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        if (!SnapshotValid(snapshot, world_, bounds_))
            return Result<PhysicsMetricPublishDisposition>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (availability_ == PhysicsMetricAvailability::Off)
            return Result<PhysicsMetricPublishDisposition>::Success(PhysicsMetricPublishDisposition::SuppressedByPolicy);
        if (availability_ == PhysicsMetricAvailability::Unavailable)
            return Result<PhysicsMetricPublishDisposition>::Success(PhysicsMetricPublishDisposition::SuppressedUnavailable);

        handles_.fixedStepDuration.Observe(snapshot.fixedStepSeconds);
        if (level_ == Telemetry::MetricCollectionLevel::Detailed) {
            handles_.stageDurations[kBroadphaseStageIndex].Observe(snapshot.broadphaseSeconds);
            handles_.stageDurations[kNarrowphaseStageIndex].Observe(snapshot.narrowphaseSeconds);
            handles_.stageDurations[kSolverStageIndex].Observe(snapshot.solverSeconds);
        }
        const std::array<double, 7> counts{static_cast<double>(snapshot.bodyCount),
                                           static_cast<double>(snapshot.sleepingBodyCount),
                                           static_cast<double>(snapshot.shapeCount),
                                           static_cast<double>(snapshot.constraintCount),
                                           static_cast<double>(snapshot.broadphasePairCount),
                                           static_cast<double>(snapshot.contactCount),
                                           static_cast<double>(snapshot.queryCount)};
        for (std::size_t index = 0; index < counts.size(); ++index)
            handles_.counts[index].Set(counts[index]);
        handles_.depths[kCommandDepthIndex].Set(static_cast<double>(snapshot.commandDepth));
        handles_.depths[kEventDepthIndex].Set(static_cast<double>(snapshot.eventDepth));
        if (snapshot.droppedEventCount != 0)
            handles_.events[kDroppedEventIndex].Add(snapshot.droppedEventCount);
        if (snapshot.overflowCount != 0)
            handles_.events[kOverflowEventIndex].Add(snapshot.overflowCount);
        return Result<PhysicsMetricPublishDisposition>::Success(PhysicsMetricPublishDisposition::Submitted);
    }

    /** @copydoc PhysicsMetricBinding::Close */
    Result<void> PhysicsMetricBinding::Close() {
        if (std::this_thread::get_id() != ownerThread_)
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        closed_ = true;
        return Result<void>::Success();
    }

    /** @copydoc PhysicsMetricBinding::World */
    PhysicsWorldId PhysicsMetricBinding::World() const noexcept {
        return world_;
    }

    /** @copydoc PhysicsMetricBinding::Revision */
    std::uint64_t PhysicsMetricBinding::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc PhysicsMetricBinding::Availability */
    PhysicsMetricAvailability PhysicsMetricBinding::Availability() const noexcept {
        return availability_;
    }

    /** @copydoc PhysicsMetricBinding::PhysicsMetricBinding */
    PhysicsMetricBinding::PhysicsMetricBinding(const PhysicsWorldId world, const std::uint64_t revision, const PhysicsMetricBounds &bounds,
                                               const PhysicsMetricAvailability availability, const Telemetry::MetricCollectionLevel level,
                                               PhysicsMetricHandles handles) noexcept
        : world_(world), revision_(revision), bounds_(bounds), availability_(availability), level_(level), handles_(std::move(handles)),
          ownerThread_(std::this_thread::get_id()) {}
}  // namespace Horo::Physics
