#pragma once

#include "Horo/Runtime/Ui/UiSceneReconciliation.h"

#include <memory>
#include <optional>

namespace Horo::Runtime::Ui::Detail {
    struct RuntimeUiServiceState;

    /** @brief Owner-thread source reservation; service keeps weak entries, derived owner keeps the only admission pin. */
    struct UiAnimationSourceReservation final {
        RuntimeUiInstanceId instance;
        UiCanvasId canvas;
        UiReloadLease generation; /**< Exact source pin fences canonical entry reclamation even before the first derived frame. */
        bool retiring{};
        bool unpublished{};
        std::weak_ptr<UiSceneReconciliation::Prepared::Storage>
            preparation; /**< Exact unpublished canonical candidate, never a caller nonce. */
    };

    /** @brief Private source access preserving either standalone ownership or the sole service-owned publisher. */
    class UiAnimationPublisher final {
    public:
        explicit UiAnimationPublisher(UiHotReload publisher) noexcept;
        UiAnimationPublisher(UiAnimationPublisher &&) noexcept = default;
        UiAnimationPublisher &operator=(UiAnimationPublisher &&) noexcept = default;
        UiAnimationPublisher(const UiAnimationPublisher &) = delete;
        UiAnimationPublisher &operator=(const UiAnimationPublisher &) = delete;

        /** @brief Reserves one exact service instance without transferring or copying its authored publisher. */
        [[nodiscard]] static Result<UiAnimationPublisher> Attach(std::shared_ptr<RuntimeUiServiceState> service,
                                                                 RuntimeUiInstanceId instance, UiCanvasId canvas);
        /** @brief Reserves an exact private incoming publisher before canonical Scene publication; abandonment revokes access. */
        [[nodiscard]] static Result<UiAnimationPublisher> AttachPrepared(std::shared_ptr<RuntimeUiServiceState> service,
                                                                         UiSceneReconciliation::Prepared &prepared,
                                                                         RuntimeUiInstanceId instance, UiCanvasId canvas);
        [[nodiscard]] bool IsAvailable() const noexcept;
        [[nodiscard]] UiReloadGeneration *Current() noexcept;
        [[nodiscard]] UiReloadGeneration *Replacement(const UiHotReload::Prepared &prepared) noexcept;
        [[nodiscard]] Result<UiReloadLease> Acquire() const;
        [[nodiscard]] bool IsCurrent(const UiReloadLease &lease) const noexcept;
        [[nodiscard]] Result<UiHotReload::Prepared> Prepare(UiReloadGeneration replacement, const CancellationToken &cancellation);
        [[nodiscard]] Result<UiReloadReconciliation> Commit(UiHotReload::Prepared &prepared, UiStructuralCommitPoint point);
        [[nodiscard]] Result<bool> ApplyPresentation(UiCanvasId canvas, const UiPresentationReceipt &receipt);
        [[nodiscard]] bool InputEligible(UiCanvasId canvas, UiRenderViewId view) const noexcept;
        void Shutdown() noexcept;
        [[nodiscard]] Result<std::size_t> CollectRetired();
        [[nodiscard]] bool CanReclaim() const noexcept;
        /** @brief Checks exact trusted service/reservation identity before synchronous coordinated reload. */
        [[nodiscard]] bool Matches(const std::shared_ptr<RuntimeUiServiceState> &service, RuntimeUiInstanceId instance) const noexcept;

    private:
        UiAnimationPublisher(std::shared_ptr<RuntimeUiServiceState> service,
                             std::shared_ptr<UiAnimationSourceReservation> reservation) noexcept;
        [[nodiscard]] static Result<UiAnimationPublisher> AttachPublisher(
            std::shared_ptr<RuntimeUiServiceState> service, RuntimeUiInstanceId instance, UiCanvasId canvas, UiHotReload &publisher,
            std::shared_ptr<UiSceneReconciliation::Prepared::Storage> prepared = {}, UiReloadLease source = {});
        [[nodiscard]] UiReloadGeneration *CandidateGeneration() const noexcept;
        [[nodiscard]] UiHotReload *Resolve() noexcept;
        [[nodiscard]] const UiHotReload *Resolve() const noexcept;
        std::optional<UiHotReload> standalone_;
        std::shared_ptr<RuntimeUiServiceState> service_;
        std::shared_ptr<UiAnimationSourceReservation> reservation_;
        std::weak_ptr<UiSceneReconciliation::Prepared::Storage>
            prepared_; /**< Exact private entry; generation pin survives revocation, not its mutation authority. */
    };

    /** @brief Checks bounded live reservations before a non-coordinated source replacement. */
    [[nodiscard]] bool HasAnimationSource(const RuntimeUiServiceState &service, RuntimeUiInstanceId instance) noexcept;
    /** @brief Clears old source attribution once after successful canonical publication. */
    void InvalidateRuntimeUiAttribution(RuntimeUiServiceState &service, RuntimeUiInstanceId instance) noexcept;
}  // namespace Horo::Runtime::Ui::Detail
