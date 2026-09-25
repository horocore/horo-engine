#pragma once

/** @file PhysicsQueryEventCapability.h
 * @brief World-bound immediate and queued query access plus completed-tick events for Physics clients.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Physics/PhysicsEvents.h"
#include "Horo/Physics/PhysicsQuery.h"

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Physics {
    class PhysicsWorld;
    struct PhysicsQueryEventCapabilityState;
    struct PhysicsQueryBatchState;
    /** @brief Maximum simultaneously usable client access states issued by one Physics world. */
    inline constexpr std::uint32_t MaximumPhysicsQueryEventCapabilitiesPerWorld = 256;
    /** @brief Hard aggregate hit-storage limit for one queued batch, independent of the query-count budget. */
    inline constexpr std::uint32_t MaximumPhysicsQueryBatchHits = 4096;

    /** @brief Exact access identity issued by one active Physics world. */
    struct PhysicsQueryEventIdentity final {
        PhysicsWorldId world;
        std::uint64_t capabilityGeneration{};
    };

    /** @brief Owned synchronous query command. Its descriptor owns all selectors and geometry. */
    struct PhysicsQueryCommand final {
        PhysicsQueryEventIdentity identity;
        std::uint64_t expectedPublicationRevision{};
        PhysicsQueryDescriptor descriptor;
    };

    /** @brief Completed immediate query with the publication observed at admission. */
    struct PhysicsQueryCompletion final {
        PhysicsQueryResult result;
        std::uint64_t completedTick{};
        std::uint64_t publicationRevision{};
    };

    /** @brief One completed query and its owned, ordered, solver-neutral hits. */
    struct PhysicsQueryBatchEntry final {
        PhysicsQueryCompletion completion;
        std::vector<PhysicsQueryHit> hits;
    };

    /** @brief Immutable all-or-nothing result in request order, independent of world lifetime. */
    struct PhysicsQueryBatchCompletion final {
        std::vector<PhysicsQueryBatchEntry> entries;
    };

    /** @brief Thread-safe terminal observation and cancellation for one admitted owner-thread batch. */
    class PhysicsQueryBatchHandle final {
    public:
        /** @brief Requests cancellation before publication. @return True if this call made the batch terminal. */
        [[nodiscard]] bool Cancel() const noexcept;
        /** @brief Reads a terminal result without waiting or accessing PhysicsWorld.
         * @return Null shared pointer while pending, immutable completed results, or a typed terminal error.
         */
        [[nodiscard]] Result<std::shared_ptr<const PhysicsQueryBatchCompletion>> Poll() const;

    private:
        friend class PhysicsQueryEventCapability;

        explicit PhysicsQueryBatchHandle(std::shared_ptr<PhysicsQueryBatchState> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<PhysicsQueryBatchState> state_;
    };

    /** @brief Request for the latest exact completed tick; older ticks are not readable through this capability. */
    struct PhysicsEventReadCommand final {
        PhysicsQueryEventIdentity identity;
        std::uint64_t completedTick{};
        std::uint64_t publicationRevision{};
        std::uint32_t maximumRecords{};
    };

    /** @brief Metadata for copied caller-owned event storage. */
    struct PhysicsEventReadCompletion final {
        std::uint32_t recordCount{};
        bool truncated{};
        std::uint32_t omittedRecordCount{}; /**< Published records omitted by this read's caller bound. */
        std::uint64_t droppedRecordCount{}; /**< Records lost earlier by bounded event projection. */
    };

    /**
     * @brief Copyable access to one world generation, with independently revocable shared state.
     *
     * The capability does not own or extend the world or solver lifetime. Issue, revoke, submit and
     * read are serialized on the Physics owner thread. A retained copy after world retirement returns
     * CapabilityStale without dereferencing the old world. Moved-from handles return an invalid identity
     * and CapabilityStale on access. Physics makes no module permission decision.
     */
    class PhysicsQueryEventCapability final {
    public:
        /** @brief Returns the issued world and capability generation. @return Inert exact identity, or invalid when moved from. */
        [[nodiscard]] PhysicsQueryEventIdentity Identity() const noexcept;
        /** @brief Executes one immediate query after access, world, revision and descriptor validation.
         * @param command Exact issued access and completed publication plus owned query intent.
         * @param hits Caller-owned bounded result storage; no borrowed native storage escapes.
         * @return Completed query or typed stale, revoked, unavailable, affinity or query error.
         */
        [[nodiscard]] Result<PhysicsQueryCompletion> Submit(const PhysicsQueryCommand &command, std::span<PhysicsQueryHit> hits) const;
        /** @brief Queues owned requests for later bounded owner-thread execution.
         * @param commands Non-empty exact-publication commands in desired result order.
         * @return Pollable handle, or a typed affinity, staleness, validation or capacity error.
         * @pre Physics owner thread outside a fixed tick; one batch may be pending per world.
         * Request count fits the world's query budgets; the sum of hit bounds fits MaximumPhysicsQueryBatchHits.
         */
        [[nodiscard]] Result<PhysicsQueryBatchHandle> SubmitBatch(std::span<const PhysicsQueryCommand> commands) const;
        /** @brief Copies records from exactly one completed tick into caller-owned storage.
         * @param command Exact issued access, latest tick and publication revision, with a non-zero result bound.
         * @param records Caller-owned output; at most maximumRecords entries are written.
         * @return Copied count, caller-omitted count and projection-drop evidence, or a typed access/tick error.
         * @pre Physics owner thread, outside a fixed-tick callback. The caller may retain copied values indefinitely.
         */
        [[nodiscard]] Result<PhysicsEventReadCompletion> ReadEvents(const PhysicsEventReadCommand &command,
                                                                    std::span<PhysicsEventRecord> records) const;

    private:
        friend class PhysicsWorld;

        explicit PhysicsQueryEventCapability(std::shared_ptr<PhysicsQueryEventCapabilityState> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<PhysicsQueryEventCapabilityState> state_;
    };
}  // namespace Horo::Physics
