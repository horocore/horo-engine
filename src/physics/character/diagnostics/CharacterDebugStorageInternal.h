#pragma once

/** @file
 * @brief Preparation-allocated, owner-thread-only probe retention with separate candidate and committed prefixes.
 */
#include "Horo/Physics/CharacterDebugSnapshot.h"
#include "Horo/Physics/CharacterWorldSettings.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace Horo::Character::Detail {
    /** @brief Source stamps travel with retained probes rather than adopting refreshed Physics revisions. */
    struct CharacterDebugTraceMetadata final {
        CharacterControllerHandle controller;
        std::uint64_t sourceTick{};
        std::uint64_t physicsSnapshotRevision{};
        std::uint32_t count{};
        std::uint32_t observed{};
        bool providerSupported{};
    };

    /** @brief Owns fixed per-slot prefixes; observation overflow cannot alter query admission or simulation.
     *
     * Preparation reserves both prefixes within maximumDebugPrimitives. Per-slot capacity is
     * min(32, floor(maximumDebugPrimitives / (2 * maximumControllers))); zero explicitly disables
     * probe retention, not movement. Metadata is bounded by controller capacity. Nothing resizes
     * or calls a consumer during a tick. Only normal publication copies candidate evidence.
     */
    class CharacterDebugStorage final {
    public:
        explicit CharacterDebugStorage(const CharacterWorldSettings &settings)
            : capacity_(std::min<std::uint32_t>(MaximumCharacterDebugProbes, settings.Values().capacities.maximumDebugPrimitives /
                                                                                 (2U * settings.Values().capacities.maximumControllers))),
              candidateMetadata_(settings.Values().capacities.maximumControllers),
              committedMetadata_(settings.Values().capacities.maximumControllers),
              candidate_(static_cast<std::size_t>(capacity_) * candidateMetadata_.size()), committed_(candidate_.size()) {}

        /** @brief Starts an observation candidate without modifying its committed predecessor. */
        void Begin(const CharacterControllerHandle controller, const bool providerSupported) noexcept {
            active_ = controller.slot.index;
            candidateMetadata_[active_] = {.controller = controller, .providerSupported = providerSupported};
        }

        /** @brief Ends the synchronous observation scope; staged entries remain private until normal commit. */
        void End() noexcept {
            active_ = NoSlot;
        }

        /** @brief Copies admitted query geometry; overflowing observation storage only increments a bounded omission count. */
        void Record(const CharacterDebugProbe &probe) noexcept {
            if (active_ == NoSlot)
                return;
            auto &metadata = candidateMetadata_[active_];
            if (metadata.observed != std::numeric_limits<std::uint32_t>::max())
                ++metadata.observed;
            if (metadata.count < capacity_)
                candidate_[Offset(active_) + metadata.count++] = probe;
        }

        /** @brief Copies the actual admitted sweep request, never reconstructing geometry from movement results. */
        void RecordSweep(const CharacterSweepProbeRequest &request, const CharacterSweepProbeResult &result,
                         const CharacterDebugProbePurpose purpose) noexcept {
            Record({purpose, CharacterDebugProbeKind::Sweep, request.capsule, request.position, request.up, request.direction,
                    request.maximumDistanceMeters, request.iteration, result.hitCount, result.truncated, request.selectors,
                    request.collisionProfile, request.queryChannel});
        }

        /** @brief Copies a completed overlap request after its owning operation has validated the evidence it consumes. */
        void RecordOverlap(const CharacterOverlapProbeRequest &request, const CharacterOverlapProbeResult &result,
                           const CharacterDebugProbePurpose purpose) noexcept {
            Record({purpose,
                    CharacterDebugProbeKind::Overlap,
                    request.capsule,
                    request.position,
                    request.up,
                    {},
                    0,
                    request.iteration,
                    result.overlapCount,
                    false,
                    request.selectors,
                    request.collisionProfile,
                    request.queryChannel});
        }

        /** @brief Adopts one already-validated controller publication's observation prefix, without allocation. */
        void Commit(const CharacterControllerHandle controller, const std::uint64_t tick, const std::uint64_t revision) noexcept {
            const auto index = controller.slot.index;
            auto metadata = candidateMetadata_[index];
            metadata.sourceTick = tick;
            metadata.physicsSnapshotRevision = revision;
            const auto offset = Offset(index);
            std::copy_n(candidate_.begin() + offset, metadata.count, committed_.begin() + offset);
            committedMetadata_[index] = metadata;
        }

        /** @brief Returns exact committed stamps on the world owner thread. */
        [[nodiscard]] const CharacterDebugTraceMetadata &Metadata(const std::uint32_t index) const noexcept {
            return committedMetadata_[index];
        }

        /** @brief Returns a bounded committed borrow valid only during synchronous owner-thread copying. */
        [[nodiscard]] std::span<const CharacterDebugProbe> Probes(const std::uint32_t index) const noexcept {
            return std::span{committed_}.subspan(Offset(index), committedMetadata_[index].count);
        }

        /** @brief Reports explicit retention enablement independently from Physics capability. */
        [[nodiscard]] std::uint32_t Capacity() const noexcept {
            return capacity_;
        }

    private:
        [[nodiscard]] std::size_t Offset(const std::uint32_t index) const noexcept {
            return static_cast<std::size_t>(index) * capacity_;
        }

        static constexpr std::uint32_t NoSlot = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t capacity_{};
        std::uint32_t active_{NoSlot};
        std::vector<CharacterDebugTraceMetadata> candidateMetadata_;
        std::vector<CharacterDebugTraceMetadata> committedMetadata_;
        std::vector<CharacterDebugProbe> candidate_;
        std::vector<CharacterDebugProbe> committed_;
    };
}  // namespace Horo::Character::Detail
