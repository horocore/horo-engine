#pragma once

/** @file NavigationCrowdDiagnostics.h
 * @brief Bounded, provider-neutral crowd capture diagnostics.
 */

#include "Horo/Navigation/NavigationCrowdSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Horo::Navigation {
    /** @brief Aggregate measurements from one immutable crowd capture. */
    struct NavigationCrowdCaptureMetrics final {
        std::size_t activeAgents{};
        std::size_t groups{};
        std::size_t neighborFacts{};
        std::uint64_t neighborsOmitted{};
        std::uint64_t boundariesOmitted{};
    };

    /** @brief Counts committed capture facts without allocating. */
    [[nodiscard]] NavigationCrowdCaptureMetrics SummarizeNavigationCrowd(const NavigationCrowdSnapshot &capture) noexcept;

    /** @brief Exact owner observation. Every field is required to reuse copied debug evidence. */
    struct NavigationCrowdDebugStamp final {
        NavigationAgentSceneBinding binding;
        NavigationGeneration topology;
        NavigationDynamicRegistryRevision dynamicRevision;
        std::uint64_t tick{};

        constexpr auto operator<=>(const NavigationCrowdDebugStamp &) const noexcept = default;
    };

    /** @brief Caller-owned presentation budget for one optional debug read. */
    struct NavigationCrowdDebugLimits final {
        static constexpr std::size_t MaximumGroups = 128;
        static constexpr std::size_t MaximumAgents = 512;
        std::size_t groups{MaximumGroups};
        std::size_t agents{MaximumAgents};

        /** @brief Checks positive limits within the published hard ceiling. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return groups != 0 && groups <= MaximumGroups && agents != 0 && agents <= MaximumAgents;
        }
    };

    /** @brief One copied agent fact row in group and stable agent order. */
    struct NavigationCrowdDebugAgent final {
        CrowdAgentHandle handle;
        Math::Vec3 position;
        Math::Vec3 velocity;
        std::uint32_t neighbors{};
        std::uint32_t neighborsOmitted{};
        std::uint32_t boundariesOmitted{};
    };

    /** @brief Spatial group and its contiguous debug-agent range. */
    struct NavigationCrowdDebugGroup final {
        std::int32_t x{};
        std::int32_t z{};
        std::size_t firstAgent{};
        std::size_t shownAgents{};
        std::size_t totalAgents{};
    };

    /** @brief Detached diagnostic frame addressed to its exact source publication. */
    struct NavigationCrowdDebugSnapshot final {
        NavigationCrowdDebugStamp stamp;
        NavigationCrowdCaptureMetrics metrics;
        std::vector<NavigationCrowdDebugGroup> groups;
        std::vector<NavigationCrowdDebugAgent> agents;
        bool truncated{};
    };

    /** @brief Owner-lane source for one immutable committed crowd capture.
     * @details Retaining a capture does not materialize debug rows. Only an explicit bounded read allocates.
     */
    class NavigationCrowdDebugSource final {
    public:
        /** @brief Pins the exact capture or clears the source for invalid input. */
        void Retain(const NavigationCrowdSnapshot &capture, NavigationGeneration topology) noexcept;
        /** @brief Retires the source and releases its capture pin. */
        void Clear() noexcept;
        /** @brief Returns the source stamp, or no stamp after retirement. */
        [[nodiscard]] std::optional<NavigationCrowdDebugStamp> Stamp() const noexcept;
        /** @brief Copies bounded per-group/agent rows only for the exact current owner publication.
         * @param current Caller-verified current Scene, topology, overlay and tick.
         * @param limits Positive debug projection bounds.
         * @return Detached rows, or a typed stale, descriptor or capacity error.
         */
        [[nodiscard]] Result<NavigationCrowdDebugSnapshot> Read(const NavigationCrowdDebugStamp &current,
                                                                NavigationCrowdDebugLimits limits = {}) const;

    private:
        NavigationCrowdSnapshot capture_;
        NavigationGeneration topology_;
    };
}  // namespace Horo::Navigation
