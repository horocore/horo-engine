#pragma once

/**
 * @file ReleaseService.h
 * @brief Shared service-owned release job submission, observation and cancellation.
 */

#include "Horo/Foundation/JobSystem.h"
#include "Horo/Release/ReleasePipelineExecutor.h"
#include "Horo/Release/ReleaseRunHistory.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Horo::Release {
    /** @brief Creates one isolated stage worker for each accepted release job. */
    class IReleaseWorkerFactory {
    public:
        virtual ~IReleaseWorkerFactory() = default;
        /** @brief Creates a worker for one frozen plan. @param plan Immutable plan. @return Owned worker or typed failure. */
        [[nodiscard]] virtual Result<std::unique_ptr<IReleasePipelineStages>> Create(const ReleaseExecutionPlan &plan) = 0;
    };

    /** @brief Finite service admission, terminal retention and stage execution policy. */
    struct ReleaseServiceConfig final {
        std::size_t activeCapacity{8};
        std::size_t recentCapacity{32};
        JobSystemConfig workers{2, 32, 32};
        ReleasePipelineLimits pipeline;
        ReleaseRunHistory *history{}; /**< Optional host-owned durable history; must outlive the service. */
        WallClock *wallClock{};       /**< Required host-owned UTC clock when history is set. */
    };

    /** @brief Durable identities returned to every GUI, CLI, MCP or CI adapter. */
    struct ReleaseSubmission final {
        ReleaseJobId job;
        ReleaseTargetId target;
        OperationId operation{};
    };

    /** @brief Sole owner of queued/running release records independent of any observer lifetime. */
    class ReleaseService final {
    public:
        /**
         * @brief Creates the release scheduler and bounded store.
         * @param operations Cross-use-case operation projection; must outlive this service.
         * @param facts Thread-safe host-owned source of fresh preflight observations; must outlive this service.
         * @param workers Thread-safe host-owned worker factory; must outlive this service.
         * @param config Finite admission, retention and execution limits.
         */
        ReleaseService(OperationStore &operations, IReleasePreflightFactsProvider &facts, IReleaseWorkerFactory &workers,
                       const ReleaseServiceConfig &config = {});
        /** @brief Cancels and joins all owned work before releasing records. */
        ~ReleaseService();
        ReleaseService(const ReleaseService &) = delete;
        ReleaseService &operator=(const ReleaseService &) = delete;

        /** @brief Submits one frozen target without binding it to the caller's lifetime.
         * @param plan Preflight-accepted immutable target. @return Durable identities or admission failure. */
        [[nodiscard]] Result<ReleaseSubmission> Submit(ReleaseExecutionPlan plan);
        /** @brief Copies one internally consistent job observation. @param job Service-owned identity.
         * @return Snapshot or none when unknown/expired. */
        [[nodiscard]] std::optional<ReleaseJobSnapshot> Query(ReleaseJobId job) const;
        /** @brief Copies all retained active and recent jobs in ID order. @return Bounded snapshots. */
        [[nodiscard]] std::vector<ReleaseJobSnapshot> List() const;
        /** @brief Copies restart-readable typed job summaries. @return Persisted records, or empty when no history is configured. */
        [[nodiscard]] std::vector<ReleaseRunHistoryEntry> ListHistory() const;
        /** @brief Copies one retained diagnostic. @param job Owning job. @param diagnostic Diagnostic identity.
         * @return Record or none when unknown/expired. */
        [[nodiscard]] std::optional<ReleaseDiagnostic> Diagnostic(ReleaseJobId job, ReleaseDiagnosticId diagnostic) const;
        /** @brief Requests explicit cancellation without waiting. @param job Service-owned identity.
         * @return Success or typed unknown/terminal failure. */
        [[nodiscard]] Result<void> RequestCancel(ReleaseJobId job);
        /** @brief Closes admission, requests cancellation and joins every accepted worker. */
        void Shutdown();

    private:
        struct Record;
        struct CancellationSlot;
        struct CancellationGate;
        /** @brief Projects one terminal and applies bounded recent retention exactly once. */
        void RecordTerminal(const std::shared_ptr<Record> &record);
        /** @brief Durably records a credential-free snapshot when a history store was supplied. */
        [[nodiscard]] Result<void> PersistSnapshot(const ReleaseJobSnapshot &snapshot) const;
        /** @brief Applies an operation cancellation request while coordinating service teardown. */
        static void CancelOperation(const std::shared_ptr<CancellationSource> &cancellation, const std::shared_ptr<CancellationSlot> &slot,
                                    const std::shared_ptr<CancellationGate> &gate);
        /** @brief Runs one accepted record and translates its authoritative terminal to a job result. */
        [[nodiscard]] Result<void> RunRecord(const std::shared_ptr<Record> &record, const CancellationToken &token);

        OperationStore &operations_;
        IReleasePreflightFactsProvider &facts_;
        IReleaseWorkerFactory &workers_;
        const ReleaseServiceConfig config_;
        JobSystem jobs_;
        std::shared_ptr<CancellationGate> cancellationGate_;
        mutable std::mutex mutex_;
        std::unordered_map<std::uint64_t, std::shared_ptr<Record>> records_;
        std::deque<std::uint64_t> recent_;
        std::size_t activeCount_{};
        std::uint64_t nextJob_{1};
        std::uint64_t nextTarget_{1};
        std::uint64_t nextCandidate_{1};
        bool closing_{};
    };
}  // namespace Horo::Release
