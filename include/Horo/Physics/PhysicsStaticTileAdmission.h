#pragma once

/** @file PhysicsStaticTileAdmission.h @brief Producer admission for versioned static collision tiles. */

#include "Horo/Math/SceneMath.h"
#include "Horo/Physics/PhysicsCookedShapeDescriptor.h"
#include "Horo/Physics/PhysicsIdentity.h"

#include <optional>
#include <vector>

namespace Horo::Physics {
    struct PhysicsStaticTileProducerTag;
    /** @brief Owner-issued producer slot scoped to a non-reused Physics authority. */
    using PhysicsStaticTileProducer = PhysicsHandle<PhysicsStaticTileProducerTag>;

    /** @brief Stable authored identity of one collision tile across content replacements. */
    struct PhysicsStaticTileKey final {
        Assets::AssetId asset;
        PhysicsShapeSubresourceId tile;
        auto operator<=>(const PhysicsStaticTileKey &) const noexcept = default;
    };

    /** @brief Immutable static geometry reference and its finite bounds in the tile's authored frame. */
    struct PhysicsStaticTileContent final {
        std::uint64_t version{}; /**< Positive producer revision, increasing across applied versions. */
        PhysicsCookedShapeDescriptor shape;
        Math::Aabb bounds;
    };

    /** @brief Producer intent, consumed later by the independently composed Physics execution owner. */
    enum class PhysicsStaticTileOperation : std::uint8_t {
        Submit,
        Replace,
        Retire
    };

    /** @brief Exact update with a compare-and-set version fence. */
    struct PhysicsStaticTileUpdate final {
        PhysicsStaticTileOperation operation{};
        PhysicsStaticTileKey key;
        std::uint64_t expectedVersion{};                 /**< Zero for first submission, otherwise the exact last applied version. */
        std::optional<PhysicsStaticTileContent> content; /**< Required for submit/replace and absent for retirement. */
    };

    /** @brief Admission and execution evidence. Accepted is never readiness. */
    enum class PhysicsStaticTileStage : std::uint8_t {
        Accepted,
        Activated,
        Retired,
        Failed,
        Cancelled
    };

    /** @brief Exact identity of one request and its state as observed at receipt creation. */
    struct PhysicsStaticTileReceipt final {
        PhysicsStaticTileProducer producer;
        PhysicsStaticTileKey key;
        std::uint64_t request{};
        std::uint64_t version{};
        PhysicsStaticTileStage stage{};
        auto operator<=>(const PhysicsStaticTileReceipt &) const noexcept = default;
    };

    /** @brief Outcome explicitly acknowledged by the execution owner, after its own safe point. */
    enum class PhysicsStaticTileOutcome : std::uint8_t {
        Applied,
        Failed,
        Cancelled
    };

    /** @brief Bounded owner-lane admission for producer updates and consumer acknowledgements.
     * @details The host retains this owner and routes producer calls by token. Only its execution composition
     * acknowledges Applied. It rechecks ReadPending at the publication safe point, with cancellation serialized
     * on the same lane. This object holds metadata and performs no Physics execution.
     */
    class PhysicsStaticTileAdmission final {
    public:
        /** @brief Creates bounded storage for one non-reused authority.
         * @param authority Unique Physics authority used to scope every producer token.
         * @param target Exact qualified cooked Physics target for submissions.
         * @param maximumProducers Positive producer-slot limit.
         * @param maximumTiles Positive limit for active, pending and retired version records together.
         * @return Admission or typed invalid/capacity failure.
         */
        [[nodiscard]] static Result<PhysicsStaticTileAdmission> Create(PhysicsWorldId authority, PhysicsShapeCookTargetDigest target,
                                                                       std::uint32_t maximumProducers, std::uint32_t maximumTiles);

        PhysicsStaticTileAdmission(const PhysicsStaticTileAdmission &) = delete;
        PhysicsStaticTileAdmission &operator=(const PhysicsStaticTileAdmission &) = delete;
        /** @brief Transfers authority and leaves the source inert. */
        PhysicsStaticTileAdmission(PhysicsStaticTileAdmission &&other) noexcept;
        PhysicsStaticTileAdmission &operator=(PhysicsStaticTileAdmission &&) = delete;

        /** @brief Opens one producer generation. @return Token or typed capacity failure. */
        [[nodiscard]] Result<PhysicsStaticTileProducer> OpenProducer();
        /** @brief Closes an owner after applied tiles retire and cancels remaining unactivated submissions.
         * @param producer Exact live ownership token.
         * @return Success, typed token failure, or InvalidState while any applied tile remains active.
         */
        [[nodiscard]] Result<void> CloseProducer(PhysicsStaticTileProducer producer);
        /** @brief Reserves one exact update.
         * @param producer Exact live ownership token. @param update Detached tile content or retirement fence.
         * @return Accepted receipt or typed refusal with all prior records intact.
         */
        [[nodiscard]] Result<PhysicsStaticTileReceipt> Admit(PhysicsStaticTileProducer producer, const PhysicsStaticTileUpdate &update);
        /** @brief Captures detached metadata for the exact pending receipt.
         * @param receipt Accepted receipt held by the execution owner.
         * @return Current update or typed stale failure.
         */
        [[nodiscard]] Result<PhysicsStaticTileUpdate> ReadPending(const PhysicsStaticTileReceipt &receipt) const;
        /** @brief Acknowledges exact execution or pre-publication cancellation.
         * @param receipt Exact still-pending receipt. @param outcome Execution owner's observed terminal outcome.
         * @return New terminal receipt or typed stale/invalid failure.
         * @pre Applied retirement is acknowledged only after the execution owner's tile leases drain.
         */
        [[nodiscard]] Result<PhysicsStaticTileReceipt> Complete(const PhysicsStaticTileReceipt &receipt, PhysicsStaticTileOutcome outcome);

    private:
        struct ProducerSlot final {
            std::uint32_t generation{1};
            bool open{};
        };

        struct Pending final {
            PhysicsStaticTileReceipt receipt;
            PhysicsStaticTileUpdate update;
        };

        struct Tile final {
            PhysicsStaticTileProducer owner;
            PhysicsStaticTileKey key;
            std::uint64_t version{}; /**< Retained after retirement to deny stale content until the owner closes. */
            bool active{};
            std::optional<Pending> pending;
        };

        PhysicsStaticTileAdmission(PhysicsWorldId authority, PhysicsShapeCookTargetDigest target, std::vector<ProducerSlot> producers,
                                   std::vector<Tile> tiles, std::uint32_t maximumTiles) noexcept;
        [[nodiscard]] Result<void> ValidateProducer(PhysicsStaticTileProducer producer) const;
        [[nodiscard]] Result<void> ValidateUpdate(const PhysicsStaticTileUpdate &update) const;

        PhysicsWorldId authority_;
        PhysicsShapeCookTargetDigest target_;
        std::vector<ProducerSlot> producers_;
        std::vector<Tile> tiles_;
        std::uint32_t maximumTiles_{};
        std::uint64_t nextRequest_{1};
    };
}  // namespace Horo::Physics
