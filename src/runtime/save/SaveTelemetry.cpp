#include "Horo/Runtime/Save/SaveTelemetry.h"

#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveOperation.h"
#include "SaveTelemetryInternal.h"

#include <atomic>
#include <format>
#include <new>

namespace Horo::Runtime {
    namespace {
        /** @brief Publishes the immutable host owner; the host retires all producers before destruction. */
        std::atomic<const SaveTelemetryRegistration *> &ActiveRegistration() {
            static std::atomic<const SaveTelemetryRegistration *> registration{};
            return registration;
        }

        /** @brief Reports an instrumentation failure without formatting private exception or ambient context. */
        void ReportObservationFailure() noexcept {
            Log::Logger::WriteEmergency("runtime.save.telemetry", Log::Level::Warn, "Save telemetry observation could not be recorded");
        }

#if HORO_ENABLE_TELEMETRY
        constexpr std::array<std::string_view, 9> StageNames{"capture", "encode", "migrate",  "commit",     "restore",
                                                             "sync",    "queue",  "recovery", "participant"};
        constexpr std::array<std::string_view, 4> OutcomeNames{"succeeded", "failed", "cancelled", "interrupted"};

        /** @brief Constructs inert fixed-cardinality descriptor metadata outside the producer path. */
        Telemetry::InstrumentDescriptor Descriptor(std::string name, const Telemetry::MetricUnit unit, const bool outcomes = false) {
            Telemetry::InstrumentDescriptor descriptor{.name = std::move(name),
                                                       .subsystem = "runtime.save",
                                                       .unit = unit,
                                                       .description = "Bounded Save pipeline observation",
                                                       .dimensions = {{.key = "stage"}},
                                                       .maxSeries = outcomes ? 36U : 9U};
            for (const auto stage : StageNames)
                descriptor.dimensions.front().allowedValues.emplace_back(stage);
            if (outcomes) {
                descriptor.dimensions.push_back({.key = "outcome"});
                for (const auto outcome : OutcomeNames)
                    descriptor.dimensions.back().allowedValues.emplace_back(outcome);
            }
            return descriptor;
        }

        /** @brief Emits one canonical safe log/span pair into the common asynchronous dispatcher. */
        void EmitObservation(const std::size_t stage, const SaveTelemetryOutcome outcome, const std::uint64_t operation,
                             const std::uint64_t parent, const std::chrono::nanoseconds duration, const SaveTelemetryEvidence &evidence) {
            const std::array fields{Telemetry::Field{"stage", std::string{StageNames[stage]}},
                                    Telemetry::Field{"outcome", std::string{OutcomeNames[static_cast<std::size_t>(outcome)]}},
                                    Telemetry::Field{"operation", operation},
                                    Telemetry::Field{"duration_ns", static_cast<std::uint64_t>(duration.count())},
                                    Telemetry::Field{"bytes", evidence.bytes},
                                    Telemetry::Field{"retries", evidence.retries},
                                    Telemetry::Field{"dropped_work", evidence.droppedWork},
                                    Telemetry::Field{"queue_depth", evidence.queueDepth},
                                    Telemetry::Field{"failure_category", static_cast<std::uint64_t>(evidence.failureCategory)}};
            const auto context = Log::LogContextSnapshot::Isolated(
                {{"save.operation", std::to_string(operation)}, {"save.parent_operation", std::to_string(parent)}});
            const auto severity = outcome == SaveTelemetryOutcome::Failed ? Log::Level::Warn : Log::Level::Info;
            {
                const Log::ScopedLogContext binding{context};
                const std::string message = std::format("Save stage {} {} operation={}", StageNames[stage],
                                                        OutcomeNames[static_cast<std::size_t>(outcome)], operation);
                // Common logging owns high-severity emergency delivery when its bounded queue rejects a record.
                Log::Logger::Write("runtime.save.stage", severity, message, fields);
            }
            auto status = Telemetry::SpanStatus::Cancelled;
            if (outcome == SaveTelemetryOutcome::Succeeded)
                status = Telemetry::SpanStatus::Succeeded;
            else if (outcome == SaveTelemetryOutcome::Failed)
                status = Telemetry::SpanStatus::Failed;
            static_cast<void>(Telemetry::Runtime::EmitRecord({.subsystem = "runtime.save",
                                                              .context = context,
                                                              .payload = Telemetry::SpanRecord{.operationId = operation,
                                                                                               .parentOperationId = parent,
                                                                                               .name = std::string{StageNames[stage]},
                                                                                               .status = status,
                                                                                               .duration = duration,
                                                                                               .fields = {fields.begin(), fields.end()}}}));
        }
#endif
    }  // namespace

    /** @copydoc SaveTelemetryRegistration::Create */
    Result<std::unique_ptr<SaveTelemetryRegistration>> SaveTelemetryRegistration::Create() {
        if (ActiveRegistration().load())
            return Result<std::unique_ptr<SaveTelemetryRegistration>>::Failure(MakeError(SaveErrors::DiagnosticInvalid));
        try {
            auto registration = std::make_unique<SaveTelemetryRegistration>(ConstructionKey{});
#if HORO_ENABLE_TELEMETRY
            using enum Telemetry::MetricUnit;
            const auto outcomes = Telemetry::Runtime::RegisterCounter(Descriptor("save.stage.outcomes", Count, true));
            const auto duration = Telemetry::Runtime::RegisterTiming(Descriptor("save.stage.duration", Seconds));
            const auto bytes = Telemetry::Runtime::RegisterHistogram(Descriptor("save.stage.bytes", Bytes));
            const auto retries = Telemetry::Runtime::RegisterCounter(Descriptor("save.stage.retries", Count));
            const auto dropped = Telemetry::Runtime::RegisterCounter(Descriptor("save.stage.dropped", Count));
            registration->queueDepth_ = Telemetry::Runtime::RegisterGauge({.name = "save.queue.depth",
                                                                           .subsystem = "runtime.save",
                                                                           .unit = Count,
                                                                           .description = "Current admitted Save queue depth"});
            for (std::size_t stage = 0; stage < Stages; ++stage) {
                const std::array dimensions{Telemetry::DimensionValue{"stage", StageNames[stage]}};
                registration->durations_[stage] = duration.WithDimensions(dimensions);
                registration->bytes_[stage] = bytes.WithDimensions(dimensions);
                registration->retries_[stage] = retries.WithDimensions(dimensions);
                registration->dropped_[stage] = dropped.WithDimensions(dimensions);
                registration->hasMetrics_ =
                    registration->hasMetrics_ || static_cast<bool>(registration->durations_[stage]) ||
                    static_cast<bool>(registration->bytes_[stage]) || static_cast<bool>(registration->retries_[stage]) ||
                    static_cast<bool>(registration->dropped_[stage]) || static_cast<bool>(registration->queueDepth_);
                for (std::size_t outcome = 0; outcome < Outcomes; ++outcome) {
                    const std::array series{dimensions.front(), Telemetry::DimensionValue{"outcome", OutcomeNames[outcome]}};
                    registration->outcomes_[stage][outcome] = outcomes.WithDimensions(series);
                    registration->hasMetrics_ = registration->hasMetrics_ || static_cast<bool>(registration->outcomes_[stage][outcome]);
                }
            }
#endif
            ActiveRegistration().store(registration.get());
            return Result<std::unique_ptr<SaveTelemetryRegistration>>::Success(std::move(registration));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<SaveTelemetryRegistration>>::Failure(MakeError(SaveErrors::OperationAllocationFailed));
        }
    }

    /** @copydoc SaveTelemetryRegistration::~SaveTelemetryRegistration */
    SaveTelemetryRegistration::~SaveTelemetryRegistration() {
        const auto *expected = this;
        static_cast<void>(ActiveRegistration().compare_exchange_strong(expected, nullptr));
    }

    /** @copydoc SaveStageObservation::SaveStageObservation */
    SaveStageObservation::SaveStageObservation(const SaveTelemetryStage stage, const std::uint64_t operation) noexcept : stage_(stage) {
#if HORO_ENABLE_TELEMETRY
        if (stage >= SaveTelemetryStage::Count || !Telemetry::Runtime::IsEnabled())
            return;
        const auto *registration = ActiveRegistration().load();
        if (!registration || (!registration->hasMetrics_ && !Telemetry::Runtime::IsEventEnabled("runtime.save", Log::Level::Warn)))
            return;
        try {
            const auto inherited = Telemetry::CaptureOperationIdentity();
            operation_ = operation != 0 ? operation : inherited.operationId;
            Telemetry::OperationContext context{.operationId = operation_,
                                                .parentOperationId = inherited.operationId == operation_ ? inherited.parentOperationId
                                                                                                         : inherited.operationId,
                                                .diagnosticContext = Log::LogContextSnapshot::Isolated(
                                                    {{"save.operation", std::to_string(operation_)},
                                                     {"save.parent_operation",
                                                      std::to_string(inherited.operationId == operation_ ? inherited.parentOperationId
                                                                                                         : inherited.operationId)}})};
            parent_ = context.parentOperationId;
            context_.emplace(context);
            started_ = std::chrono::steady_clock::now();
            registration_ = registration;
        } catch (...) {
            ReportObservationFailure();
        }
#else
        static_cast<void>(operation);
#endif
    }

    /** @copydoc SaveStageObservation::~SaveStageObservation */
    SaveStageObservation::~SaveStageObservation() {
        Complete(SaveTelemetryOutcome::Interrupted);
    }

    /** @copydoc SaveStageObservation::Complete */
    void SaveStageObservation::Complete(const SaveTelemetryOutcome outcome, const SaveTelemetryEvidence &evidence) noexcept {
        if (!registration_ || completed_ || outcome >= SaveTelemetryOutcome::Count)
            return;
        completed_ = true;
        const auto stage = static_cast<std::size_t>(stage_);
        const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_);
        registration_->outcomes_[stage][static_cast<std::size_t>(outcome)].Add();
        registration_->durations_[stage].Record(duration);
        registration_->bytes_[stage].Observe(static_cast<double>(evidence.bytes));
        registration_->retries_[stage].Add(evidence.retries);
        registration_->dropped_[stage].Add(evidence.droppedWork);
        if (stage_ == SaveTelemetryStage::Queue)
            registration_->queueDepth_.Set(static_cast<double>(evidence.queueDepth));
#if HORO_ENABLE_TELEMETRY
        try {
            EmitObservation(stage, outcome, operation_, parent_, duration, evidence);
        } catch (...) {
            ReportObservationFailure();
        }
#endif
    }

    /** @copydoc SaveStageObservation::Fail */
    void SaveStageObservation::Fail(const Error &error, const SaveTelemetryEvidence &evidence) noexcept {
        if (!registration_ || completed_)
            return;
        auto failureEvidence = evidence;
        try {
            const auto diagnostic = MakeSaveDiagnosticRecord(error);
            if (diagnostic.HasValue())
                failureEvidence.failureCategory = diagnostic.Value().Category();
        } catch (...) {
            ReportObservationFailure();
        }
        Complete(failureEvidence.failureCategory == SaveFailureCategory::Cancellation ? SaveTelemetryOutcome::Cancelled
                                                                                      : SaveTelemetryOutcome::Failed,
                 failureEvidence);
    }

    /** @copydoc RecordSaveOperationTerminal */
    void RecordSaveOperationTerminal(const SaveOperationSnapshot &snapshot) noexcept {
#if HORO_ENABLE_TELEMETRY
        if (!Telemetry::Runtime::IsEventEnabled("runtime.save", Log::Level::Info) || !ActiveRegistration().load())
            return;
        try {
            auto category = SaveFailureCategory::Count;
            if (snapshot.terminalError) {
                const auto diagnostic = MakeSaveDiagnosticRecord(*snapshot.terminalError);
                if (diagnostic.HasValue())
                    category = diagnostic.Value().Category();
            }
            const std::array fields{Telemetry::Field{"operation", snapshot.operation},
                                    Telemetry::Field{"kind", static_cast<std::uint64_t>(snapshot.kind)},
                                    Telemetry::Field{"state", static_cast<std::uint64_t>(snapshot.state)},
                                    Telemetry::Field{"stage", static_cast<std::uint64_t>(snapshot.stage)},
                                    Telemetry::Field{"commit", static_cast<std::uint64_t>(snapshot.commit)},
                                    Telemetry::Field{"cancellation_reason", static_cast<std::uint64_t>(snapshot.cancellationReason)},
                                    Telemetry::Field{"failure_category", static_cast<std::uint64_t>(category)}};
            const auto context = Log::LogContextSnapshot::Isolated({{"save.operation", std::to_string(snapshot.operation)}});
            static_cast<void>(Telemetry::Runtime::EmitRecord({.subsystem = "runtime.save",
                                                              .context = context,
                                                              .payload = Telemetry::LogRecord{.severity = Log::Level::Info,
                                                                                              .category = "runtime.save.operation",
                                                                                              .message = "Save operation terminal outcome",
                                                                                              .fields = {fields.begin(), fields.end()}}}));
        } catch (...) {
            ReportObservationFailure();
        }
#else
        static_cast<void>(snapshot);
#endif
    }

}  // namespace Horo::Runtime
