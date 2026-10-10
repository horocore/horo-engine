#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiAnimationOwnerInternal.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Closes animation-owner reentry for quiescent collection, including exception unwinding from foreign authority cleanup. */
        class DrainAdmissionGuard final {
        public:
            explicit DrainAdmissionGuard(bool &draining) noexcept : draining_(draining) {
                draining = true;
            }

            ~DrainAdmissionGuard() {
                draining_ = false;
            }

            DrainAdmissionGuard(const DrainAdmissionGuard &) = delete;
            DrainAdmissionGuard &operator=(const DrainAdmissionGuard &) = delete;
            DrainAdmissionGuard(DrainAdmissionGuard &&) = delete;
            DrainAdmissionGuard &operator=(DrainAdmissionGuard &&) = delete;

        private:
            bool &draining_;
        };
    }  // namespace

    /** @copydoc UiAnimationOwner::UiAnimationOwner */
    UiAnimationOwner::UiAnimationOwner(std::shared_ptr<Storage> storage) noexcept {
        storage_.PublisherPin() = std::move(storage);
    }

    UiAnimationOwner::~UiAnimationOwner() {
        if (storage_ && storage_->pointerDispatching)
            std::terminate();  // Synchronous routed callbacks still borrow this public owner object.
        Shutdown();
    }

    UiAnimationOwner::UiAnimationOwner(UiAnimationOwner &&other) noexcept {
        if (other.storage_ && other.storage_->pointerDispatching)
            std::terminate();
        storage_ = std::move(other.storage_);
    }

    UiAnimationOwner &UiAnimationOwner::operator=(UiAnimationOwner &&other) noexcept {
        if ((storage_ && storage_->pointerDispatching) || (other.storage_ && other.storage_->pointerDispatching))
            std::terminate();
        if (this != &other) {
            Shutdown();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiAnimationOwner::Acquire */
    Result<UiAnimationFrameLease> UiAnimationOwner::Acquire() const {
        if (!storage_ || storage_->stopped || storage_->draining || !storage_->currentFrame.has_value())
            return Result<UiAnimationFrameLease>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        return Result<UiAnimationFrameLease>::Success(UiAnimationFrameLease{storage_->frames[*storage_->currentFrame]});
    }

    /** @copydoc UiAnimationOwner::IsCurrent */
    bool UiAnimationOwner::IsCurrent(const UiAnimationFrameLease &frame) const noexcept {
        return storage_ && !storage_->stopped && !storage_->draining && storage_->currentFrame.has_value() &&
               frame.storage_ == storage_->frames[*storage_->currentFrame] && storage_->publisher.IsCurrent(frame.storage_->generation);
    }

    /** @copydoc UiAnimationOwner::Shutdown */
    void UiAnimationOwner::Shutdown() noexcept {
        if (!storage_ || storage_->stopped)
            return;
        storage_->stopped = true;
        storage_->candidate.admitted = false;
        AbandonControlSources(*storage_);
        storage_->candidate.styles.reset();
        storage_->candidate.layout.reset();
        storage_->candidate.clipping.reset();
        storage_->candidate.source = {};
        CancelRouteValidated(*storage_, UiAnimationCancellation::Shutdown);
        storage_->publisher.Shutdown();
        if (storage_->resolver.State() == UiStyleResolverState::Active)
            (void)storage_->resolver.BeginRetirement();
        if (storage_->registry.State() == RuntimeStyleRegistryState::Active)
            (void)storage_->registry.BeginRetirement();
    }

    /** @copydoc UiAnimationOwner::DrainRetired */
    Result<std::size_t> UiAnimationOwner::DrainRetired() {
        if (!storage_ || storage_->draining || storage_->candidate.admitted || storage_->pointerDispatching)
            return Result<std::size_t>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        // The application calls this outside frame work; owned frame slots retain snapshots until external leases drain.
        if (storage_->stopped) {
            for (const auto &frame : storage_->frames) {
                if (frame.use_count() != 1)
                    continue;
                frame->styles.reset();
                frame->layout.reset();
                frame->clipped.reset();
                frame->generation = {};
            }
        }
        std::size_t reclaimed{};
        if (auto *generation = storage_->publisher.Current()) {
            for (const auto &canvas : generation->Canvases()) {
                auto *owned = generation->Canvas(canvas.id);
                if (owned->actions)
                    reclaimed += owned->actions->DrainInteractionReplacement();
                for (auto &element : owned->controls)
                    reclaimed += element.control.DrainInteractionReplacement();
                if (!owned->routes)
                    continue;
                const auto drained = owned->routes->DrainRetiredActions();
                if (drained.HasError())
                    return Result<std::size_t>::Failure(drained.ErrorValue());
                reclaimed += drained.Value();
            }
        }
        const DrainAdmissionGuard admission{storage_->draining};
        const auto collected = storage_->publisher.CollectRetired();
        if (collected.HasError())
            return Result<std::size_t>::Failure(collected.ErrorValue());
        return Result<std::size_t>::Success(reclaimed + collected.Value());
    }

    /** @copydoc UiAnimationOwner::CanReclaim */
    bool UiAnimationOwner::CanReclaim() const noexcept {
        return !storage_ || (storage_->stopped && storage_.UseCount() == 1 && !storage_->candidate.admitted &&
                             storage_->resolver.IsDrained() && std::ranges::all_of(storage_->frames, [](const auto &frame) {
            return frame.use_count() == 1;
        }) && storage_->publisher.CanReclaim());
    }

    /** @copydoc UiAnimationOwner::Prepared::Prepared */
    UiAnimationOwner::Prepared::Prepared(std::shared_ptr<UiAnimationOwner::Storage> owner, const std::uint32_t slot,
                                         const std::uint64_t sourceRevision) noexcept
        : owner_(std::move(owner)), slot_(slot), sourceRevision_(sourceRevision), admitted_(true) {}

    UiAnimationOwner::Prepared::~Prepared() {
        Abandon();
    }

    UiAnimationOwner::Prepared::Prepared(Prepared &&other) noexcept
        : owner_(std::move(other.owner_)), slot_(other.slot_), sourceRevision_(other.sourceRevision_),
          admitted_(std::exchange(other.admitted_, false)) {}

    UiAnimationOwner::Prepared &UiAnimationOwner::Prepared::operator=(Prepared &&other) noexcept {
        if (this != &other) {
            Abandon();
            owner_ = std::move(other.owner_);
            slot_ = other.slot_;
            sourceRevision_ = other.sourceRevision_;
            admitted_ = std::exchange(other.admitted_, false);
        }
        return *this;
    }

    /** @copydoc UiAnimationOwner::Prepared::Abandon */
    void UiAnimationOwner::Prepared::Abandon() noexcept {
        if (!admitted_)
            return;
        admitted_ = false;
        if (owner_->candidate.admitted && owner_->candidate.frameSlot == slot_ && owner_->candidate.commandRevision == sourceRevision_) {
            owner_->candidate.admitted = false;
            UiAnimationOwner::AbandonControlSources(*owner_);
            owner_->candidate.styles.reset();
            owner_->candidate.layout.reset();
            owner_->candidate.clipping.reset();
            owner_->candidate.source = {};
        }
    }
}  // namespace Horo::Runtime::Ui
