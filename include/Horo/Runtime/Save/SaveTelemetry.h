#pragma once

/** @file SaveTelemetry.h
 * @brief Explicit host registration and bounded privacy-safe Save instrumentation.
 */
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Telemetry/Operation.h"
#include "Horo/Runtime/Save/SaveDiagnostics.h"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace Horo::Runtime {
    /** @brief Closed production Save stage taxonomy; identities never become metric dimensions. */
    enum class SaveTelemetryStage : std::uint8_t {
        Capture,
        Encode,
        Migrate,
        Commit,
        Restore,
        Sync,
        Queue,
        Recovery,
        Participant,
        Count
    };
    /** @brief Closed stage result taxonomy independent of the operation's authoritative result. */
    enum class SaveTelemetryOutcome : std::uint8_t {
        Succeeded,
        Failed,
        Cancelled,
        Interrupted,
        Count
    };

    /** @brief Safe scalar stage evidence; no names, paths, payloads, account IDs or provider text are accepted. */
    struct SaveTelemetryEvidence final {
        std::uint64_t bytes{};
        std::uint64_t retries{};
        std::uint64_t droppedWork{};
        std::uint64_t queueDepth{};
        SaveFailureCategory failureCategory{SaveFailureCategory::Count};
    };

    /** @brief Derives a bounded safe summary exclusively from retained canonical Save log records.
     * @param retainedLogs Explicit host-owned JSONL sources; at most 32 sources, 2,048 rows and 8 MiB are inspected.
     * @return Canonical save.summary metadata containing numeric stage/outcome totals and incomplete-input counts;
     * malformed/oversized rows are omitted, and no source path, raw text or private context is copied.
     */
    [[nodiscard]] Result<std::pair<std::string, std::string>> SummarizeSaveTelemetry(std::span<const std::filesystem::path> retainedLogs);

    class SaveStageObservation;

    /**
     * @brief Host-owned registration of the Save contribution into the existing process telemetry runtime.
     * @details Create after observability startup, outside latency-sensitive loops. Exactly one host registration is allowed.
     * The host must join/retire every Save producer before destroying this owner, then shut observability down.
     * Do not restart or enable a different telemetry generation while this immutable registration is alive.
     * Producer threads borrow immutable bound handles; registration never selects sinks or owns an event store.
     */
    class SaveTelemetryRegistration final {
        /** @brief Factory-only key keeping inert allocation inaccessible to hosts. */
        struct ConstructionKey final {
            explicit ConstructionKey() = default;
        };

    public:
        /** @brief Creates inert storage with a factory-only key; only Create can supply the key.
         * @param key Unforgeable admission key owned by the registration factory.
         */
        explicit SaveTelemetryRegistration(ConstructionKey key) noexcept {
            static_cast<void>(key);
        }

        /** @brief Registers fixed descriptors and bounded stage/outcome series.
         * @return Owned registration or a typed invalid/allocation error. Disabled builds return an inert owner.
         */
        [[nodiscard]] static Result<std::unique_ptr<SaveTelemetryRegistration>> Create();
        /** @brief Closes registration after every producer has retired; no blocking drain occurs. */
        ~SaveTelemetryRegistration();
        SaveTelemetryRegistration(const SaveTelemetryRegistration &) = delete;
        SaveTelemetryRegistration &operator=(const SaveTelemetryRegistration &) = delete;

    private:
        friend class SaveStageObservation;
        static constexpr std::size_t Stages = static_cast<std::size_t>(SaveTelemetryStage::Count);
        static constexpr std::size_t Outcomes = static_cast<std::size_t>(SaveTelemetryOutcome::Count);
        std::array<std::array<Telemetry::Counter, Outcomes>, Stages> outcomes_;
        std::array<Telemetry::Timing, Stages> durations_;
        std::array<Telemetry::Histogram, Stages> bytes_;
        std::array<Telemetry::Counter, Stages> retries_;
        std::array<Telemetry::Counter, Stages> dropped_;
        Telemetry::Gauge queueDepth_;
        bool hasMetrics_{};
    };

    /**
     * @brief Stack-owned stage observation with canonical numeric context and bounded record submission.
     * @details Disabled observations perform no clock reads, formatting, context capture or allocation. Enabled scopes
     * bind only safe operation identity, so jobs inherit numeric Save correlation through Foundation JobSystem.
     * Normal delivery never retries, flushes or waits on a producer. Failed stages use the common logger WARN path,
     * including its bounded synchronous emergency reporting when queued delivery rejects the record. Emit outside operation
     * locks; the registration owner must outlive this scope.
     */
    class SaveStageObservation final {
    public:
        /** @brief Begins one stage only when explicitly registered and process telemetry is enabled.
         * @param stage Closed stage identity.
         * @param operation Existing authoritative Save operation identity, or zero to inherit current logical work.
         */
        explicit SaveStageObservation(SaveTelemetryStage stage, std::uint64_t operation = 0) noexcept;
        /** @brief Records Interrupted for a scope whose normal result was never observed. */
        ~SaveStageObservation();
        SaveStageObservation(const SaveStageObservation &) = delete;
        SaveStageObservation &operator=(const SaveStageObservation &) = delete;

        /** @brief Reports whether optional evidence collection is admitted. @return True only for an enabled registered scope. */
        [[nodiscard]] bool IsActive() const noexcept {
            return registration_ != nullptr;
        }

        /** @brief Records one terminal observation; duplicate calls are harmless.
         * @param outcome Closed stage outcome, never parsed to determine application success.
         * @param evidence Allowlisted scalar evidence; no private data is admitted.
         */
        void Complete(SaveTelemetryOutcome outcome, const SaveTelemetryEvidence &evidence = {}) noexcept;
        /** @brief Projects only the canonical typed failure category, discarding all private error text.
         * @param error Authoritative error retained unchanged by the caller.
         * @param evidence Allowlisted scalar stage observations.
         */
        void Fail(const Error &error, const SaveTelemetryEvidence &evidence = {}) noexcept;

    private:
        const SaveTelemetryRegistration *registration_{};
        SaveTelemetryStage stage_{};
        std::uint64_t operation_{};
        std::uint64_t parent_{};
        std::chrono::steady_clock::time_point started_{};
        std::optional<Telemetry::ScopedOperationContext> context_;
        bool completed_{};
    };

    /** @brief Observes one existing typed operation without translating or swallowing its result.
     * @param stage Closed stage identity.
     * @param operation Existing Save operation correlation.
     * @param work Synchronous work returning the original typed Result; may schedule jobs under the bound context.
     * @param evidence Safe scalar observations known at admission.
     * @return Unmodified application result. Exceptions propagate unchanged after Interrupted instrumentation.
     */
    template <typename Work>
    [[nodiscard]] auto ObserveSaveStage(const SaveTelemetryStage stage, const std::uint64_t operation, Work &&work,
                                        const SaveTelemetryEvidence &evidence = {}) {
        SaveStageObservation observation{stage, operation};
        auto result = std::forward<Work>(work)();
        if (result.HasError())
            observation.Fail(result.ErrorValue(), evidence);
        else
            observation.Complete(SaveTelemetryOutcome::Succeeded, evidence);
        return result;
    }
}  // namespace Horo::Runtime
