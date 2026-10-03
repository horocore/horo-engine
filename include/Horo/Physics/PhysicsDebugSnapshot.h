#pragma once

/** @file PhysicsDebugSnapshot.h
 * @brief Bounded, owned Physics observations copied at a completed-tick boundary.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Physics/PhysicsEvents.h"
#include "Horo/Physics/PhysicsIdentity.h"
#include "Horo/Physics/PhysicsPose.h"
#include "Horo/Physics/PhysicsQuery.h"
#include "Horo/Physics/PhysicsTickPipeline.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace Horo::Physics {
    /** @brief Closed record categories. Values match the alternatives of PhysicsDebugRecord. */
    enum class PhysicsDebugCategory : std::uint8_t {
        Body,
        Shape,
        Contact,
        Constraint,
        Broadphase,
        Query,
        Pipeline,
        Count
    };
    inline constexpr std::size_t PhysicsDebugCategoryCount = static_cast<std::size_t>(PhysicsDebugCategory::Count);
    inline constexpr std::uint32_t MaximumPhysicsDebugRecords = 4096;
    inline constexpr std::uint32_t MaximumPhysicsDebugPayloadBytes = 1'048'576;

    /** @brief Copied body identity; pose and sleep state require a separate current-state projection. */
    struct PhysicsDebugBody final {
        BodyHandle body;
    };

    /** @brief Copied shape identity and its owning body, if one exists. */
    struct PhysicsDebugShape final {
        ShapeHandle shape;
        BodyHandle body;
    };

    /** @brief Copied contact or trigger event; no manifold or callback view escapes. */
    struct PhysicsDebugContact final {
        PhysicsEventRecord event;
    };

    /** @brief Copied authored constraint identity and endpoint identities. */
    struct PhysicsDebugConstraint final {
        ConstraintHandle constraint;
        BodyHandle first;
        BodyHandle second;
    };

    /** @brief Copied broadphase candidate pair in stable Horo identities. */
    struct PhysicsDebugBroadphase final {
        BodyHandle first;
        BodyHandle second;
    };

    /** @brief Copied completed query hit and its schema generation. */
    struct PhysicsDebugQuery final {
        PhysicsQueryHit hit;
    };

    /** @brief Actual completed-tick publication counts; this does not claim stage timing support. */
    struct PhysicsDebugPipeline final {
        std::uint32_t appliedCommands{};
        std::uint32_t eventCount{};
        std::uint64_t droppedEventCount{};
    };

    /** @brief One owned, backend-neutral category record. */
    using PhysicsDebugRecord = std::variant<PhysicsDebugBody, PhysicsDebugShape, PhysicsDebugContact, PhysicsDebugConstraint,
                                            PhysicsDebugBroadphase, PhysicsDebugQuery, PhysicsDebugPipeline>;

    /** @brief Per-category source availability, distinct from an available empty observation. */
    enum class PhysicsDebugAvailability : std::uint8_t {
        Unavailable,
        Available
    };

    /** @brief One borrowed category batch valid only during CapturePhysicsDebugSnapshot. */
    struct PhysicsDebugSourceCategory final {
        PhysicsDebugAvailability availability{PhysicsDebugAvailability::Unavailable};
        std::span<const PhysicsDebugRecord> records;
        std::uint64_t truncatedBeforeCapture{}; /**< Source records omitted by the producer's capture budget. */
        std::uint64_t droppedBeforeCapture{};   /**< Producer-side losses before this bounded copy. */
    };

    /** @brief One exact completed-tick source; category records are borrowed for the call only. */
    struct PhysicsDebugSource final {
        PhysicsWorldId world;
        std::uint64_t simulationTick{};
        std::uint64_t publicationRevision{};
        std::array<PhysicsDebugSourceCategory, PhysicsDebugCategoryCount> categories;
    };

    /** @brief Per-category record and retained variant-storage byte limits; zero disables that category. */
    struct PhysicsDebugCategoryBudget final {
        std::uint32_t maximumRecords{};
        std::uint32_t maximumPayloadBytes{};
    };

    /** @brief Opt-in limits for one capture, including an aggregate payload cap. */
    struct PhysicsDebugBudget final {
        std::array<PhysicsDebugCategoryBudget, PhysicsDebugCategoryCount> categories;
        std::uint32_t maximumPayloadBytes{};
    };

    /** @brief Owned evidence about one category's source and bounded copy. */
    struct PhysicsDebugCategoryEvidence final {
        PhysicsDebugAvailability availability{PhysicsDebugAvailability::Unavailable};
        std::uint32_t captured{};
        std::uint64_t truncated{}; /**< Valid source records omitted by this capture's limits. */
        std::uint64_t dropped{};   /**< Records lost by the source before capture. */
        std::uint32_t payloadBytes{};
    };

    /**
     * @brief Immutable after publication; retains only copied Horo values and stable identities.
     *
     * A shared_ptr<const PhysicsDebugSnapshot> may outlive the source spans, later world ticks,
     * world reset, and destruction. Compare world, tick, and revision with a current publication
     * marker to detect staleness; these value identities do not keep the world alive.
     */
    class PhysicsDebugSnapshot final {
    public:
        /** @brief Returns the captured world identity. */
        [[nodiscard]] PhysicsWorldId World() const noexcept {
            return world_;
        }

        /** @brief Returns the captured simulation tick. */
        [[nodiscard]] std::uint64_t SimulationTick() const noexcept {
            return simulationTick_;
        }

        /** @brief Returns the captured publication revision. */
        [[nodiscard]] std::uint64_t PublicationRevision() const noexcept {
            return publicationRevision_;
        }

        /** @brief Returns total copied record-storage bytes, excluding fixed snapshot metadata. */
        [[nodiscard]] std::uint32_t PayloadBytes() const noexcept {
            return payloadBytes_;
        }

        /** @brief Returns copied records for a known category; an unknown value returns an empty view. */
        [[nodiscard]] std::span<const PhysicsDebugRecord> Records(PhysicsDebugCategory category) const noexcept;
        /** @brief Returns per-category capture evidence; unknown values return unavailable evidence. */
        [[nodiscard]] PhysicsDebugCategoryEvidence Evidence(PhysicsDebugCategory category) const noexcept;
        /** @brief Checks exact world/tick/revision identity without touching a world or native state. */
        [[nodiscard]] bool Matches(PhysicsWorldId world, const PhysicsPublishedTick &published) const noexcept;

    private:
        friend Result<std::shared_ptr<const PhysicsDebugSnapshot>> CapturePhysicsDebugSnapshot(const PhysicsDebugSource &,
                                                                                               const PhysicsPublishedTick &,
                                                                                               const PhysicsDebugBudget &);
        PhysicsWorldId world_;
        std::uint64_t simulationTick_{};
        std::uint64_t publicationRevision_{};
        std::uint32_t payloadBytes_{};
        std::array<PhysicsDebugCategoryEvidence, PhysicsDebugCategoryCount> evidence_;
        std::array<std::vector<PhysicsDebugRecord>, PhysicsDebugCategoryCount> records_;
    };

    /**
     * @brief Copies exact completed-tick source data into one bounded immutable snapshot.
     * @param source Borrowed per-category values from the owner-thread completed-tick phase.
     * @param published Current coherent world publication marker.
     * @param budget Positive aggregate and per-category record-storage byte limits; zero category limits filter it out.
     * @return Retained immutable value or a typed stale, malformed, or out-of-profile budget error.
     * @pre Call only after successful tick publication and before the next tick or world mutation.
     * The owning PhysicsWorld method enforces thread, lifecycle and current-publication checks.
     * @post Failure publishes nothing; source storage is never retained.
     */
    [[nodiscard]] Result<std::shared_ptr<const PhysicsDebugSnapshot>> CapturePhysicsDebugSnapshot(const PhysicsDebugSource &source,
                                                                                                  const PhysicsPublishedTick &published,
                                                                                                  const PhysicsDebugBudget &budget);
}  // namespace Horo::Physics
