#pragma once
#include "Horo/Runtime/Ui/UiSceneReconciliation.h"
#include "UiHotReloadInternal.h"

#include <algorithm>

namespace Horo::Runtime::Ui {
    struct UiSceneReconciliation::Storage final {
        struct Entry final {
            UiSceneInstanceDescriptor descriptor;
            UiHotReload publisher;
            bool sceneRevoked{};
            bool providerRevoked{};
        };

        explicit Storage(UiSceneReconciliationLimits bounds)
            : limits(bounds), active(bounds.maximumInstances), retired(bounds.maximumRetiredInstances + bounds.maximumInstances),
              preparations(bounds.maximumPreparedTransitions), issued(bounds.previousInstanceSlot) {}

        UiSceneReconciliationLimits limits;
        std::vector<std::optional<Entry>> active;
        std::vector<std::optional<Entry>> retired;
        std::vector<std::weak_ptr<UiSceneReconciliation::Prepared::Storage>> preparations;
        std::uint32_t issued{};
        std::uint64_t revision{1};
        std::uint32_t preparedCount{};
        bool stopped{};
        bool collecting{};
        bool shutdownRequested{};
        const void *publicationSeal{}; /**< Identity of the one owner-thread service publication token, never user supplied. */
        void Stop() noexcept;
        void FlushSceneRevocations() noexcept;

        [[nodiscard]] std::size_t Find(RuntimeUiInstanceId instance) const noexcept {
            for (std::size_t i = 0; i < active.size(); ++i)
                if (active[i] && active[i]->descriptor.instance == instance)
                    return i;
            return active.size();
        }

        [[nodiscard]] std::size_t FreeRetired() const noexcept {
            return static_cast<std::size_t>(std::ranges::count_if(retired, [](const auto &entry) {
                return !entry;
            }));
        }

        [[nodiscard]] Result<void> Validate(const UiSceneInstanceDescriptor &descriptor, const UiReloadGeneration &generation) const;
        [[nodiscard]] Result<void> Issue(const UiSceneInstanceDescriptor &descriptor);
    };

    struct UiSceneReconciliation::Prepared::Storage final {
        Storage() = default;
        Storage(const Storage &) = delete;
        Storage &operator=(const Storage &) = delete;
        Storage(Storage &&) = delete;
        Storage &operator=(Storage &&) = delete;

        struct Retire final {
            std::size_t slot{};
            UiReloadLease source;
            std::vector<UiReloadDetail::CanvasStamp> stamps;
        };

        struct Rebind final {
            std::size_t slot{};
            UiHotReload::Prepared candidate;
        };

        std::shared_ptr<UiSceneReconciliation::Storage> owner;
        UiSceneTransitionRequest request;
        CancellationToken cancellation;
        std::uint64_t revision{};
        UiSceneReconciliationResult result;
        std::vector<Retire> retiring;
        std::vector<Rebind> rebinding;
        std::vector<UiSceneReconciliation::Storage::Entry> incoming;
        std::vector<std::shared_ptr<Detail::UiAnimationSourceReservation>> animationSources;
        std::vector<std::weak_ptr<Detail::UiAnimationSourceReservation>> preparedAnimations;
        bool consumed{};
        bool abandoned{}; /**< A facade abandonment revokes derived candidate access even while its generation remains pinned. */
        bool admitted{};

        /** @brief Pins one exact outgoing publisher and its complete source stamps before retirement. */
        [[nodiscard]] Horo::Result<void> PrepareRetirement(std::size_t slot);
        /** @brief Prepares only a detached replacement while retaining the live publisher unchanged. */
        [[nodiscard]] Horo::Result<void> PrepareRebind(std::size_t slot, UiSceneBindingReplacement &replacement);
        /** @brief Prepares affected active owners and rejects missing or extra replacement identities. */
        [[nodiscard]] Horo::Result<void> PrepareActive(std::vector<UiSceneBindingReplacement> &bindings);
        /** @brief Protects incoming admission and the reserved shutdown retirement capacity. */
        [[nodiscard]] Horo::Result<void> ValidateCapacity(std::size_t incomingCount) const;
        /** @brief Qualifies a complete incoming Scene publisher without admitting it to live slots. */
        [[nodiscard]] Horo::Result<void> PrepareIncoming(UiSceneActivation &activation);

        ~Storage() {
            if (admitted)
                --owner->preparedCount;
        }
    };

    namespace UiSceneDetail {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        [[nodiscard]] inline bool Cutoff(UiStructuralCommitPoint point) noexcept {
            return point == UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands ||
                   point == UiStructuralCommitPoint::CommitDeferredLifecycleChanges;
        }
    }  // namespace UiSceneDetail
}  // namespace Horo::Runtime::Ui
