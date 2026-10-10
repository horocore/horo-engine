#pragma once

/** @file UiScreenTransition.h
 * @brief Asynchronous preparation and atomic publication of complete Runtime UI screens.
 */

#include "Horo/Runtime/Ui/UiHotReload.h"

#include <cstdint>
#include <memory>

namespace Horo::Runtime::Ui {
    /** @brief Exact owner-scoped transition identity; late commands cannot target a retry. */
    struct UiScreenTransitionId final {
        UiOwnershipGeneration ownership;
        std::uint64_t sequence{};
        [[nodiscard]] constexpr auto operator<=>(const UiScreenTransitionId &) const noexcept = default;
    };
    /** @brief Observable stages of one screen transition; terminal failure preserves the last-good screen. */
    enum class UiScreenTransitionState : std::uint8_t {
        Idle,
        LoadingDocument,
        LoadingDependencies,
        LoadCompleted,
        AwaitingComposition,
        Ready,
        Committed,
        Failed,
        Cancelled,
        TimedOut,
        Stopped
    };

    /** @brief Allocation-free publication outcome; refusal preserves current and candidate ownership. */
    enum class UiScreenTransitionCommitResult : std::uint8_t {
        Committed,
        NotReady,
        InvalidPoint,
        SourceStale,
        RouteBusy,
        Collecting,
        RetentionFull,
        Stopped
    };

    /** @brief Value-only progress; stages describe actual work rather than an estimated percentage. */
    struct UiScreenTransitionProgress final {
        UiScreenTransitionId operation; /**< Never-reused owner-local sequence; empty before the first request. */
        UiScreenTransitionState state{UiScreenTransitionState::Idle};
        std::uint64_t elapsedTicks{};
        std::uint64_t timeoutTicks{};
    };

    /** @brief Complete request copied at admission; clocks use host-supplied monotonic ticks. */
    struct UiScreenTransitionRequest final {
        UiRuntimeAssetLoadRequest asset;
        RuntimeUiInstanceId instance; /**< Same owner, strictly increasing slot across all admitted screens. */
        UiRouteId route;              /**< Actual cooked route to activate in the selected canvas. */
        std::uint64_t timeoutTicks{}; /**< Positive deadline covering loading, composition and commit. */
    };

    /** @brief Fixed publisher retention bounds reserved at creation. */
    struct UiScreenTransitionLimits final {
        std::uint32_t maximumRetiredScreens{4};
        UiHotReloadLimits reload;
    };

    namespace UiScreenTransitionErrors {
        /** @brief Invalid request, identity, bound or nonmonotonic clock. */
        extern const ErrorCodeDescriptor Invalid;
        /** @brief An operation or unreclaimed candidate is still owned. */
        extern const ErrorCodeDescriptor Busy;
        /** @brief The request exceeded its finite deadline. */
        extern const ErrorCodeDescriptor Timeout;
    }  // namespace UiScreenTransitionErrors

    /**
     * @brief Owns one current whole-screen publisher, one private candidate and bounded retired publishers.
     * @details All methods run on the serialized Runtime UI owner thread. The borrowed asset loader must outlive this owner.
     * Begin, Prepare, Cancel and CollectRetired are load-time operations. Poll and Commit never pump assets, perform I/O,
     * invoke host code, allocate or reclaim generations. Hosts advance the existing loader outside frame work and construct
     * detached actual canvas owners; Prepare consumes the loader's exact closure into UiReloadGeneration. Input eligibility
     * remains governed by UiHotReload presentation receipts. Same-screen reload uses Current()->Prepare/Commit.
     */
    class UiScreenTransition final {
    public:
        /** @brief Reserves bounded ownership storage with no initial screen.
         * @param loader Borrowed existing asset loader, used only by Begin.
         * @param ownership Host-issued owner generation shared by every admitted instance.
         * @param limits Positive finite retention limits.
         * @return Owner or typed identity/budget failure.
         */
        [[nodiscard]] static Result<UiScreenTransition> Create(UiRuntimeAssetLoadService &loader, UiOwnershipGeneration ownership,
                                                               UiScreenTransitionLimits limits = {});
        ~UiScreenTransition();
        UiScreenTransition(UiScreenTransition &&) noexcept;
        UiScreenTransition &operator=(UiScreenTransition &&) noexcept;
        UiScreenTransition(const UiScreenTransition &) = delete;
        UiScreenTransition &operator=(const UiScreenTransition &) = delete;

        /** @brief Starts a load while retaining the current screen and its exact route guard.
         * @param request Exact asset, fresh instance, target route and positive timeout.
         * @param now Monotonic owner-clock tick.
         * @param cancellation Cooperative parent token checked through commit.
         * @return Never-reused operation sequence or typed admission failure.
         * @pre Load-time operation; previous operation storage has been collected.
         */
        [[nodiscard]] Result<UiScreenTransitionId> Begin(UiScreenTransitionRequest request, std::uint64_t now,
                                                         const CancellationToken &cancellation = {});
        /** @brief Observes load stage, cancellation and deadline without advancing or cancelling provider work.
         * @param now Nondecreasing owner-clock tick; a backwards tick fails the operation closed.
         * @return Value-only progress. Terminal states never change except explicit Shutdown.
         * @details Deferred cancellation and candidate destruction occur in Cancel/CollectRetired outside frame work.
         */
        [[nodiscard]] UiScreenTransitionProgress Poll(std::uint64_t now) noexcept;
        /** @brief Consumes the terminal loader result outside frame work, preserving original error information.
         * @param operation Exact identity returned by Begin; stale commands preserve the current operation.
         * @param now Nondecreasing owner-clock tick.
         * @return Complete immutable assets available for composition, or typed failure/not-ready.
         */
        [[nodiscard]] Result<void> PrepareAssets(UiScreenTransitionId operation, std::uint64_t now);
        /** @brief Borrows the exact loaded document for detached host composition.
         * @return Document after PrepareAssets, or null; borrow ends at Prepare/CollectRetired/owner destruction.
         */
        [[nodiscard]] const CookedUiDocument *LoadedDocument() const noexcept;
        /** @brief Consumes the completed closure and validates a detached complete owner composition.
         * @param operation Exact identity returned by Begin.
         * @param canvases Actual private owners covering the exact cooked document; no borrowed owner escapes preparation.
         * @param now Nondecreasing owner-clock tick.
         * @return Ready or typed load, composition, lifecycle or deadline failure; current screen is preserved.
         * @pre Load-time operation; construct trees from the requested document identity/revision and fresh instance.
         */
        [[nodiscard]] Result<void> Prepare(UiScreenTransitionId operation, std::vector<UiReloadCanvas> canvases, std::uint64_t now);
        /** @brief Publishes the complete candidate and closes old input admission at an ADR-073 cutoff.
         * @param operation Exact identity returned by Begin.
         * @param point Structural owner safe point.
         * @param now Nondecreasing owner-clock tick.
         * @return Success or typed stale, deadline, lifecycle or retention failure; refusal preserves both publishers.
         */
        [[nodiscard]] UiScreenTransitionCommitResult Commit(UiScreenTransitionId operation, UiStructuralCommitPoint point,
                                                            std::uint64_t now) noexcept;
        /** @brief Cancels the operation exactly once while preserving current ownership.
         * @param operation Exact identity returned by Begin; stale cancellation cannot affect a retry.
         * @return Success or typed lifecycle failure.
         * @pre Load-time operation; provider cancellation may synchronize with loader work.
         */
        [[nodiscard]] Result<void> Cancel(UiScreenTransitionId operation);
        /** @brief Borrows the current publisher for one owner operation, including existing typed reload/presentation commands.
         * @return Current publisher or null before boot/after shutdown; never retain across Commit or collection.
         */
        [[nodiscard]] UiHotReload *Current() noexcept;
        /** @brief Pins the current complete generation for render/input extraction.
         * @return Existing whole-generation lease or typed unavailable failure.
         */
        [[nodiscard]] Result<UiReloadLease> Acquire() const;
        /** @brief Checks the publication fence for one retained generation.
         * @param lease Complete generation lease.
         * @return Whether this is the current screen's authoritative generation.
         */
        [[nodiscard]] bool IsCurrent(const UiReloadLease &lease) const noexcept;
        /** @brief Returns the latest value-only progress. @return Last observed progress. */
        [[nodiscard]] UiScreenTransitionProgress Progress() const noexcept;
        /** @brief Borrows terminal failure detail including original loader/composition errors.
         * @return Error or null; borrow ends at the next owner command.
         */
        [[nodiscard]] const Error *Failure() const noexcept;
        /** @brief Cancels deferred work, destroys rejected candidates and collects drained retired/current reload generations.
         * @return Number of reclaimed screen publishers or typed drain failure.
         * @pre Explicit load-time operation, including after Shutdown. Terminal operation storage is cleared for retry.
         */
        [[nodiscard]] Result<std::size_t> CollectRetired();
        /** @brief Closes admission and retires publishers without joining jobs, allocating or reclaiming storage.
         * @details Reentrant shutdown during collection is applied when that collection operation exits.
         */
        void Shutdown() noexcept;
        /** @brief Checks drain after shutdown. @return Whether leases, candidates and publishers have been collected. */
        [[nodiscard]] bool CanReclaim() const noexcept;

    private:
        struct Storage;
        explicit UiScreenTransition(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
