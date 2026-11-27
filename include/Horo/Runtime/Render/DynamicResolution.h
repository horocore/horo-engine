#pragma once

/** @file DynamicResolution.h
 * @brief Owner-thread timing policy and frame-boundary publication for render extents.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/TemporalHistory.h"

#include <chrono>
#include <optional>
#include <thread>

namespace Horo::Render {
    /** @brief Independent low and high GPU budgets with bounded scale, consecutive samples and settling time. */
    struct DynamicResolutionPolicy final {
        std::chrono::nanoseconds raiseBelow{};
        std::chrono::nanoseconds lowerAbove{};
        std::uint16_t minimumPermille{500};
        std::uint16_t maximumPermille{1000};
        std::uint16_t stepPermille{50};
        std::uint16_t raiseAfter{12};
        std::uint16_t lowerAfter{3};
        std::uint16_t settlingSamples{8};

        /** @brief Verifies ordered positive budgets, bounded scale and a finite sample policy. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief One published resolution epoch; target extent belongs to the display owner. */
    struct DynamicResolutionState final {
        RenderViewId view;
        FramebufferExtent targetExtent;
        FramebufferExtent renderExtent;
        std::uint64_t generation{};
        std::uint16_t scalePermille{};

        [[nodiscard]] constexpr bool operator==(const DynamicResolutionState &) const noexcept = default;
    };

    /** @brief Completed real-frame GPU timing attributed to the exact published render generation. */
    struct DynamicResolutionSample final {
        RenderViewId view;
        std::uint64_t frameId{};
        std::uint64_t generation{};
        std::chrono::nanoseconds elapsed{};
    };

    /** @brief Budget condition that triggered an extent change. */
    enum class DynamicResolutionReason : std::uint8_t {
        BelowBudget,
        AboveBudget
    };

    /** @brief One proposal held until matching resource publication or rejection at a frame boundary. */
    struct DynamicResolutionChange final {
        DynamicResolutionState candidate;
        std::uint64_t triggerFrame{};
        DynamicResolutionReason reason{};

        [[nodiscard]] constexpr bool operator==(const DynamicResolutionChange &) const noexcept = default;
    };

    /** @brief Bounded diagnostics, including the last qualified sample from the current generation. */
    struct DynamicResolutionSnapshot final {
        DynamicResolutionState active;
        std::optional<DynamicResolutionChange> pending;
        std::optional<DynamicResolutionSample> lastSample;
        std::uint64_t publishedChanges{};
        std::uint64_t rejectedChanges{};
    };

    /**
     * @brief Converts completed GPU timing into changes proposed at the host's resource boundary.
     * @details The controller belongs to its construction thread and never allocates render resources, waits
     * on a queue or changes presentation extent. The host prepares textures and history for the candidate,
     * then resolves the exact proposal. Only success establishes a new render generation.
     */
    class DynamicResolutionController final {
    public:
        DynamicResolutionController() noexcept = default;
        DynamicResolutionController(const DynamicResolutionController &) = delete;
        DynamicResolutionController &operator=(const DynamicResolutionController &) = delete;
        DynamicResolutionController(DynamicResolutionController &&) = delete;
        DynamicResolutionController &operator=(DynamicResolutionController &&) = delete;

        /** @brief Binds the initial committed mode once.
         * @param view Frontend-issued identity of the view that owns these render generations.
         * @param policy Complete authored timing and scale limits.
         * @param targetExtent Current committed output extent.
         * @param initialPermille Scale of the initial render generation.
         * @return Success or a typed thread, lifecycle or configuration failure. */
        [[nodiscard]] Result<void> Configure(RenderViewId view, const DynamicResolutionPolicy &policy, FramebufferExtent targetExtent,
                                             std::uint16_t initialPermille);

        /** @brief Consumes one completed real-frame GPU duration.
         * @param sample Arrival must advance in frame order within the active resolution generation.
         * @return A newly proposed change, no change, or a typed invalid, stale or owner failure. */
        [[nodiscard]] Result<std::optional<DynamicResolutionChange>> Observe(const DynamicResolutionSample &sample);

        /** @brief Commits or rejects the exact pending request.
         * @param change Unmodified proposal returned by Observe.
         * @param published True only when the next extent and history policy have been published.
         * @return Success or a typed stale, owner or lifecycle failure. */
        [[nodiscard]] Result<void> Resolve(const DynamicResolutionChange &change, bool published);

        /** @brief Accepts a host output resize already committed at its resource boundary.
         * @param targetExtent New committed display size. A real resize invalidates pending work and timing.
         * @return Success or a typed validation, exhaustion, owner or lifecycle failure. */
        [[nodiscard]] Result<void> UpdateOutputExtent(FramebufferExtent targetExtent);

        /** @brief Projects the committed scale onto a prospective output size without changing the active generation.
         * @param targetExtent Requested display size.
         * @return Projected scene extent or a typed lifecycle, owner or configuration failure. */
        [[nodiscard]] Result<FramebufferExtent> PreviewOutputExtent(FramebufferExtent targetExtent) const;

        /** @brief Copies current diagnostics on the owner thread. @return Live state or typed owner/lifecycle failure. */
        [[nodiscard]] Result<DynamicResolutionSnapshot> Snapshot() const;

        /** @brief Cancels queued policy work and closes admission. @return Success or typed owner failure. */
        [[nodiscard]] Result<void> Stop();

    private:
        enum class Phase : std::uint8_t {
            Unconfigured,
            Active,
            Stopped
        };
        [[nodiscard]] Result<void> RequireActive() const;
        [[nodiscard]] std::optional<DynamicResolutionReason> AccumulateTiming(std::chrono::nanoseconds elapsed) noexcept;
        void ResetEvidence() noexcept;
        [[nodiscard]] std::uint16_t NextScale(DynamicResolutionReason reason) const noexcept;

        const std::thread::id owner_{std::this_thread::get_id()};
        Phase phase_{Phase::Unconfigured};
        DynamicResolutionPolicy policy_;
        DynamicResolutionSnapshot state_;
        std::optional<DynamicResolutionReason> direction_;
        std::uint16_t consecutive_{};
        std::uint16_t settling_{};
    };

    namespace DynamicResolutionErrors {
        extern const ErrorCodeDescriptor InvalidConfiguration;
        extern const ErrorCodeDescriptor InvalidTiming;
        extern const ErrorCodeDescriptor StaleTiming;
        extern const ErrorCodeDescriptor StaleChange;
        extern const ErrorCodeDescriptor InvalidState;
        extern const ErrorCodeDescriptor WrongThread;
        extern const ErrorCodeDescriptor GenerationExhausted;
    }  // namespace DynamicResolutionErrors
}  // namespace Horo::Render
