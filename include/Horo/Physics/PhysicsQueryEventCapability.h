#pragma once

/** @file PhysicsQueryEventCapability.h
 * @brief World-bound synchronous query and completed-tick event access for Physics clients.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Physics/PhysicsEvents.h"
#include "Horo/Physics/PhysicsQuery.h"

#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace Horo::Physics {
    class PhysicsWorld;
    struct PhysicsQueryEventCapabilityState;
    /** @brief Maximum simultaneously usable client access states issued by one Physics world. */
    inline constexpr std::uint32_t MaximumPhysicsQueryEventCapabilitiesPerWorld = 256;

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
        std::uint64_t droppedRecordCount{};
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
        /** @brief Copies records from exactly one completed tick into caller-owned storage.
         * @param command Exact issued access, latest tick and publication revision, with a non-zero result bound.
         * @param records Caller-owned output; at most maximumRecords entries are written.
         * @return Copied count and explicit truncation/drop evidence, or a typed access/tick error.
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
