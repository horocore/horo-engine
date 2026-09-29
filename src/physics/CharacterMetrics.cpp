#include "Horo/Physics/CharacterMetrics.h"

#include "Horo/Physics/CharacterErrors.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Character {
    namespace {
        constexpr std::array<std::string_view, 4> kCountValues{"controllers", "queries", "iterations", "contacts"};
        constexpr std::array<std::string_view, 2> kEventValues{"overflows", "failures"};
        constexpr std::array<std::string_view, 3> kPhaseValues{"freeze_commands", "resolve_movement", "publish_completed_tick"};

        [[nodiscard]] Telemetry::InstrumentDescriptor Descriptor(const Telemetry::InstrumentKind kind, std::string name,
                                                                 const Telemetry::MetricUnit unit, std::string description,
                                                                 const Telemetry::MetricCollectionLevel level,
                                                                 std::vector<Telemetry::DimensionDescriptor> dimensions = {}) {
            const auto maximumSeries = dimensions.empty() ? 1U : static_cast<std::uint32_t>(dimensions.front().allowedValues.size());
            return {.kind = kind,
                    .name = std::move(name),
                    .subsystem = "character",
                    .unit = unit,
                    .description = std::move(description),
                    .dimensions = std::move(dimensions),
                    .maxSeries = maximumSeries,
                    .minimumCollectionLevel = level};
        }

        template <std::size_t Size>
        [[nodiscard]] Telemetry::DimensionDescriptor Dimension(const std::string_view key,
                                                               const std::array<std::string_view, Size> &values) {
            Telemetry::DimensionDescriptor result{.key = std::string{key}};
            result.allowedValues.reserve(Size);
            for (const auto value : values)
                result.allowedValues.emplace_back(value);
            return result;
        }

        template <typename Handle, std::size_t Size>
        void Bind(std::array<Handle, Size> &handles, const Handle &root, const std::string_view dimension,
                  const std::array<std::string_view, Size> &values) {
            for (std::size_t index{}; index < Size; ++index) {
                const std::array selected{Telemetry::DimensionValue{dimension, values[index]}};
                handles[index] = root.WithDimensions(selected);
            }
        }

        template <typename Handle, std::size_t Size> [[nodiscard]] bool AllBound(const std::array<Handle, Size> &handles) noexcept {
            return std::ranges::all_of(handles, [](const Handle &handle) {
                return static_cast<bool>(handle);
            });
        }
    }  // namespace

    /** @copydoc RegisterCharacterMetricHandles */
    CharacterMetricHandles RegisterCharacterMetricHandles(const Telemetry::MetricCollectionLevel level) {
        CharacterMetricHandles handles;
        if (level == Telemetry::MetricCollectionLevel::Off || level > Telemetry::MetricCollectionLevel::Detailed)
            return handles;
        auto counts = Telemetry::Runtime::RegisterGauge(
            Descriptor(Telemetry::InstrumentKind::Gauge, "horo.character.count", Telemetry::MetricUnit::Count,
                       "Character fixed-tick item counts.", Telemetry::MetricCollectionLevel::Core, {Dimension("kind", kCountValues)}));
        Bind(handles.counts, counts, "kind", kCountValues);
        auto events =
            Telemetry::Runtime::RegisterCounter(Descriptor(Telemetry::InstrumentKind::Counter, "horo.character.event",
                                                           Telemetry::MetricUnit::Count, "Character overflow and failure attempts.",
                                                           Telemetry::MetricCollectionLevel::Core, {Dimension("kind", kEventValues)}));
        Bind(handles.events, events, "kind", kEventValues);
        if (level == Telemetry::MetricCollectionLevel::Detailed) {
            auto phases = Telemetry::Runtime::RegisterHistogram(
                Descriptor(Telemetry::InstrumentKind::Histogram, "horo.character.phase.duration", Telemetry::MetricUnit::Seconds,
                           "Character fixed-tick phase duration.", Telemetry::MetricCollectionLevel::Detailed,
                           {Dimension("phase", kPhaseValues)}));
            Bind(handles.phaseDurations, phases, "phase", kPhaseValues);
        }
        return handles;
    }

    /** @copydoc CharacterMetricBinding::Create */
    Result<CharacterMetricBinding> CharacterMetricBinding::Create(
        const CharacterWorldId world, const std::uint64_t sceneGeneration, const std::uint64_t revision,
        const std::uint32_t maximumControllers, const std::uint32_t maximumQueriesPerTick, const std::uint32_t maximumMovementIterations,
        const std::uint32_t maximumContactsPerMovement, const Telemetry::MetricCollectionLevel level, CharacterMetricHandles handles) {
        if (!world.IsValid() || sceneGeneration == 0 || revision == 0 || maximumControllers == 0 || maximumQueriesPerTick == 0 ||
            maximumMovementIterations == 0 || maximumContactsPerMovement == 0 || level > Telemetry::MetricCollectionLevel::Detailed)
            return Result<CharacterMetricBinding>::Failure(MakeError(CharacterErrors::DescriptorInvalid));
        if (level != Telemetry::MetricCollectionLevel::Off &&
            (!AllBound(handles.counts) || !AllBound(handles.events) ||
             (level == Telemetry::MetricCollectionLevel::Detailed && !AllBound(handles.phaseDurations))))
            return Result<CharacterMetricBinding>::Failure(MakeError(CharacterErrors::OperationUnsupported));
        return Result<CharacterMetricBinding>::Success(CharacterMetricBinding{world, sceneGeneration, revision, maximumControllers,
                                                                              maximumQueriesPerTick, maximumMovementIterations,
                                                                              maximumContactsPerMovement, level, std::move(handles)});
    }

    /** @copydoc CharacterMetricBinding::Publish */
    Result<void> CharacterMetricBinding::Publish(const CharacterMetricSnapshot &snapshot, const std::uint64_t expectedRevision) {
        if (std::this_thread::get_id() != ownerThread_ || closed_)
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        if (expectedRevision != revision_ || snapshot.world != world_ || snapshot.sceneGeneration != sceneGeneration_ ||
            snapshot.tick == 0 || snapshot.tick <= lastTick_)
            return Result<void>::Failure(MakeError(CharacterErrors::QuerySnapshotStale));
        const std::uint64_t maximumIterations = static_cast<std::uint64_t>(maximumQueriesPerTick_) * maximumMovementIterations_;
        const std::uint64_t maximumContacts = static_cast<std::uint64_t>(maximumControllers_) * maximumContactsPerMovement_;
        if (snapshot.activeControllers > maximumControllers_ || snapshot.queries > maximumQueriesPerTick_ ||
            snapshot.movementIterations > maximumIterations || snapshot.movementIterations > snapshot.queries ||
            snapshot.contacts > maximumContacts || snapshot.overflows < lastOverflows_ ||
            (snapshot.failed ? snapshot.publicationRevision != 0
                             : snapshot.publicationRevision <= lastPublicationRevision_ || !std::ranges::all_of(snapshot.phaseCompleted,
                                                                                                                [](const bool complete) {
            return complete;
        })) ||
            !std::ranges::all_of(snapshot.phaseSeconds, [](const double seconds) {
            return std::isfinite(seconds) && seconds >= 0;
        }))
            return Result<void>::Failure(MakeError(CharacterErrors::DescriptorInvalid));
        lastTick_ = snapshot.tick;
        if (!snapshot.failed)
            lastPublicationRevision_ = snapshot.publicationRevision;
        const std::uint64_t newOverflows = snapshot.overflows - lastOverflows_;
        lastOverflows_ = snapshot.overflows;
        if (level_ == Telemetry::MetricCollectionLevel::Off)
            return Result<void>::Success();
        const std::array<double, 4> counts{static_cast<double>(snapshot.activeControllers), static_cast<double>(snapshot.queries),
                                           static_cast<double>(snapshot.movementIterations), static_cast<double>(snapshot.contacts)};
        for (std::size_t index{}; index < counts.size(); ++index)
            handles_.counts[index].Set(counts[index]);
        if (newOverflows != 0)
            handles_.events[0].Add(newOverflows);
        if (snapshot.failed)
            handles_.events[1].Add(1);
        if (level_ == Telemetry::MetricCollectionLevel::Detailed) {
            for (std::size_t index{}; index < snapshot.phaseSeconds.size(); ++index)
                if (snapshot.phaseCompleted[index])
                    handles_.phaseDurations[index].Observe(snapshot.phaseSeconds[index]);
        }
        return Result<void>::Success();
    }

    /** @copydoc CharacterMetricBinding::Close */
    Result<void> CharacterMetricBinding::Close() {
        if (std::this_thread::get_id() != ownerThread_)
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        closed_ = true;
        return Result<void>::Success();
    }

    /** @copydoc CharacterMetricBinding::CharacterMetricBinding */
    CharacterMetricBinding::CharacterMetricBinding(const CharacterWorldId world, const std::uint64_t sceneGeneration,
                                                   const std::uint64_t revision, const std::uint32_t maximumControllers,
                                                   const std::uint32_t maximumQueriesPerTick, const std::uint32_t maximumMovementIterations,
                                                   const std::uint32_t maximumContactsPerMovement,
                                                   const Telemetry::MetricCollectionLevel level, CharacterMetricHandles handles) noexcept
        : world_(world), sceneGeneration_(sceneGeneration), revision_(revision), maximumControllers_(maximumControllers),
          maximumQueriesPerTick_(maximumQueriesPerTick), maximumMovementIterations_(maximumMovementIterations),
          maximumContactsPerMovement_(maximumContactsPerMovement), level_(level), handles_(std::move(handles)),
          ownerThread_(std::this_thread::get_id()) {}
}  // namespace Horo::Character
