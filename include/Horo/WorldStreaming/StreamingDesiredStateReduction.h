#pragma once

/**
 * @file StreamingDesiredStateReduction.h
 * @brief Deterministic reduction of overlapping world-streaming source requests.
 */

#include "Horo/WorldStreaming/StreamingDesiredState.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::WorldStreaming {
    /** @brief Exact cell and mounted-partition lifetime for one source-reduction snapshot. */
    struct StreamingDesiredStateReductionContext final {
        WorldPartitionId partition{}; /**< Stable partition receiving the reduced demand. */
        PartitionEpoch epoch{};       /**< Exact mounted partition incarnation. */
        StreamingCellId cell{};       /**< Canonical cell whose source demands are combined. */

        /** @brief Checks structural representation, not descriptor membership. @return Whether all identity fields are usable. */
        [[nodiscard]] bool IsValid() const noexcept {
            return partition.IsValid() && epoch.IsValid() && cell.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const StreamingDesiredStateReductionContext &) const noexcept = default;
    };

    /** @brief Mandatory host ceiling applied before a reduction result is published. */
    struct StreamingDesiredStateReductionLimits final {
        std::uint32_t maximumContributors{}; /**< Maximum distinct source identities contributing to one cell. */
    };

    /** @brief Minimal exact provenance retained for deterministic removal and replacement recomputation. */
    struct StreamingDesiredStateContribution final {
        StreamingSourceId source{};            /**< Stable source identity. */
        StreamingSourceOwnerToken owner{};     /**< Exact owner lifetime admitted into the snapshot. */
        StreamingSourceRevision revision{};    /**< Exact immutable source revision. */
        StreamingDesiredResidency residency{}; /**< Residency requested by this source. */
        StreamingRetention retention{};        /**< Independent retention requested by this source. */

        [[nodiscard]] constexpr auto operator<=>(const StreamingDesiredStateContribution &) const noexcept = default;
    };

    /** @brief Allocation-free commutative residency/retention accumulator shared by owned and staged reductions. */
    struct StreamingDesiredStateSummary final {
        StreamingDesiredResidency effectiveResidency{StreamingDesiredResidency::Unloaded};
        std::optional<StreamingDesiredResidency> pinnedResidencyFloor{};
    };

    /**
     * @brief Folds one already validated source request into the canonical residency/retention algebra.
     * @param summary Previous reduction; an empty summary is the identity element.
     * @param desiredState Structurally validated source request; identity admission remains the owner's responsibility.
     * @return Order-independent strongest residency and strongest independently pinned floor without allocation.
     */
    [[nodiscard]] StreamingDesiredStateSummary CombineStreamingDesiredState(StreamingDesiredStateSummary summary,
                                                                            const StreamingSourceDesiredState &desiredState) noexcept;

    /** @brief Owned immutable, order-independent desired-state reduction for one cell. */
    class StreamingDesiredStateReduction final {
    public:
        StreamingDesiredStateReduction(const StreamingDesiredStateReduction &) = delete;
        StreamingDesiredStateReduction &operator=(const StreamingDesiredStateReduction &) = delete;
        StreamingDesiredStateReduction(StreamingDesiredStateReduction &&) noexcept = default;
        StreamingDesiredStateReduction &operator=(StreamingDesiredStateReduction &&) = delete;

        /**
         * @brief Canonicalizes and combines validated source requests without mutating source or cell state.
         * @param context Exact partition incarnation and cell receiving the demand.
         * @param desiredStates Validated source requests; empty means explicit no-demand state.
         * @param limits Mandatory non-zero contributor ceiling.
         * @return Owned reduction, or a stable typed error with no partial result.
         * @details Effective residency is the strongest request. PinnedResidencyFloor is computed only from pinned contributors, so a
         * stronger releasable request cannot silently promote a weaker pinned guarantee. Priority ordering is outside this contract.
         */
        [[nodiscard]] static Result<StreamingDesiredStateReduction> Create(const StreamingDesiredStateReductionContext &context,
                                                                           std::span<const StreamingSourceDesiredState> desiredStates,
                                                                           StreamingDesiredStateReductionLimits limits);

        /** @brief Returns the exact reduction scope. @return Immutable context value. */
        [[nodiscard]] constexpr const StreamingDesiredStateReductionContext &Context() const noexcept {
            return context_;
        }

        /** @brief Returns the strongest residency requested by any contributor. @return Unloaded for explicit no demand. */
        [[nodiscard]] constexpr StreamingDesiredResidency EffectiveResidency() const noexcept {
            return effectiveResidency_;
        }

        /**
         * @brief Returns the strongest exact residency guaranteed by pinned contributors.
         * @return Empty when no source pins this cell; otherwise Loaded or Activated.
         */
        [[nodiscard]] constexpr std::optional<StreamingDesiredResidency> PinnedResidencyFloor() const noexcept {
            return pinnedResidencyFloor_;
        }

        /** @brief Returns contributors in stable source-identity order. @return Immutable result-owned provenance. */
        [[nodiscard]] std::span<const StreamingDesiredStateContribution> Contributors() const noexcept {
            return contributors_;
        }

    private:
        /** @brief Stores already validated canonical reduction state. */
        StreamingDesiredStateReduction(const StreamingDesiredStateReductionContext &context, StreamingDesiredResidency effectiveResidency,
                                       std::optional<StreamingDesiredResidency> pinnedResidencyFloor,
                                       std::vector<StreamingDesiredStateContribution> contributors) noexcept;

        StreamingDesiredStateReductionContext context_{};
        StreamingDesiredResidency effectiveResidency_{StreamingDesiredResidency::Unloaded};
        std::optional<StreamingDesiredResidency> pinnedResidencyFloor_;
        std::vector<StreamingDesiredStateContribution> contributors_;
    };
}  // namespace Horo::WorldStreaming
