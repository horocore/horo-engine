#include "AndroidWindowBridge.h"

#include "AndroidWindowErrors.h"
#include "Horo/Platform/PlatformErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Platform::Android {
    namespace {
        /** @brief Prevent native callback reentry from observing or mutating a partial transaction. */
        class OperationScope final {
        public:
            explicit OperationScope(bool &active) noexcept : active_(active) {
                active_ = true;
            }

            ~OperationScope() {
                active_ = false;
            }

            OperationScope(const OperationScope &) = delete;
            OperationScope &operator=(const OperationScope &) = delete;

        private:
            bool &active_;
        };
    }  // namespace

    Result<void> AndroidWindowBridge::CheckAccess() const {
        auto state = controller_.Snapshot();
        if (state.HasError())
            return Result<void>::Failure(std::move(state).ErrorValue());
        if (operationActive_)
            return Result<void>::Failure(MakeError(AndroidWindowErrors::OperationInProgress));
        return Result<void>::Success();
    }

    /** @copydoc AndroidWindowBridge::Shutdown */
    Result<bool> AndroidWindowBridge::Shutdown() {
        if (auto access = CheckAccess(); access.HasError())
            return Result<bool>::Failure(std::move(access).ErrorValue());
        const OperationScope operation(operationActive_);
        closing_ = true;
        observed_ = nullptr;
        const auto state = controller_.Snapshot();
        if (state.Value().process != AndroidProcessState::ShuttingDown && state.Value().process != AndroidProcessState::Stopped) {
            if (auto queued = adapter_.OnFinalShutdown(); queued.HasError())
                return Result<bool>::Failure(std::move(queued).ErrorValue());
        }
        if (auto drained = DrainLifecycle(); drained.HasError())
            return Result<bool>::Failure(std::move(drained).ErrorValue());
        return Result<bool>::Success(std::ranges::none_of(entries_, [](const Entry &entry) {
            return entry.reference != nullptr;
        }));
    }

    Result<void> AndroidWindowBridge::CheckAdmission() const {
        if (auto access = CheckAccess(); access.HasError())
            return access;
        const auto state = controller_.Snapshot();
        if (closing_ || state.Value().process == AndroidProcessState::ShuttingDown || state.Value().process == AndroidProcessState::Stopped)
            return Result<void>::Failure(MakeError(PlatformErrors::LifecycleAdmissionClosed));
        return Result<void>::Success();
    }

    /** @copydoc AndroidWindowLease::AndroidWindowLease */
    AndroidWindowLease::AndroidWindowLease(AndroidWindowBridge &owner, const std::size_t slot) noexcept : owner_(&owner), slot_(slot) {}

    /** @copydoc AndroidWindowLease::AndroidWindowLease */
    AndroidWindowLease::AndroidWindowLease(AndroidWindowLease &&other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), slot_(other.slot_) {}

    /** @copydoc AndroidWindowLease::operator= */
    AndroidWindowLease &AndroidWindowLease::operator=(AndroidWindowLease &&other) noexcept {
        if (this != &other) {
            Reset();
            owner_ = std::exchange(other.owner_, nullptr);
            slot_ = other.slot_;
        }
        return *this;
    }

    /** @copydoc AndroidWindowLease::~AndroidWindowLease */
    AndroidWindowLease::~AndroidWindowLease() {
        Reset();
    }

    /** @copydoc AndroidWindowLease::Reset */
    void AndroidWindowLease::Reset() noexcept {
        if (auto *owner = std::exchange(owner_, nullptr))
            owner->ReleasePin(slot_);
    }

    /** @copydoc AndroidWindowLease::Get */
    ANativeWindow *AndroidWindowLease::Get() const noexcept {
        if (!owner_)
            return nullptr;
        HORO_INVARIANT_MSG(std::this_thread::get_id() == owner_->ownerThread_, "Native window leases belong to the host owner thread.");
        return owner_->entries_[slot_].reference.get();
    }

    void AndroidWindowBridge::ReleasePin(const std::size_t slot) noexcept {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == ownerThread_, "Release native window leases on the host owner thread.");
        HORO_INVARIANT_MSG(!operationActive_, "Native dispatch must not release an existing window lease reentrantly.");
        const OperationScope operation(operationActive_);
        auto &entry = entries_[slot];
        HORO_INVARIANT_MSG(entry.pins == 1, "A window attachment owns exactly one native pin.");
        entry.pins = 0;
        if (entry.retired)
            entry.reference.reset();
    }

    /** @copydoc AndroidWindowBridge::AcquireWindow */
    Result<AndroidWindowLease> AndroidWindowBridge::AcquireWindow(const AndroidWindowGeneration window) {
        if (auto access = CheckAdmission(); access.HasError())
            return Result<AndroidWindowLease>::Failure(std::move(access).ErrorValue());
        const OperationScope operation(operationActive_);
        auto state = controller_.Snapshot();
        if (state.HasError())
            return Result<AndroidWindowLease>::Failure(std::move(state).ErrorValue());
        if (!observed_ || observed_->generation != window || !state.Value().hasWindow || state.Value().windowGeneration != window ||
            state.Value().activityGeneration != observed_->activity)
            return Result<AndroidWindowLease>::Failure(MakeError(PlatformErrors::StaleLifecycleGeneration));
        if (observed_->pins != 0)
            return Result<AndroidWindowLease>::Failure(MakeError(PlatformErrors::LifecycleQueueSaturated));
        observed_->pins = 1;
        return Result<AndroidWindowLease>::Success(AndroidWindowLease(*this, static_cast<std::size_t>(observed_ - entries_.data())));
    }

    /** @copydoc AndroidWindowBridge::AndroidWindowBridge */
    AndroidWindowBridge::AndroidWindowBridge(AndroidLifecycleAdapter &adapter, const AndroidWindowSelection selection,
                                             const AndroidWindowOperations operations) noexcept
        : controller_(adapter.controller_), adapter_(adapter), selection_(selection), operations_(operations) {}

    /** @copydoc AndroidWindowBridge::~AndroidWindowBridge */
    AndroidWindowBridge::~AndroidWindowBridge() {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == ownerThread_, "Destroy native window owners on their host owner thread.");
        for (const auto &entry : entries_)
            HORO_INVARIANT_MSG(entry.pins == 0, "Drain native window consumers before destroying their platform owner.");
        operationActive_ = true;
        observed_ = nullptr;
        for (auto &entry : entries_)
            entry.reference.reset();
    }

    /** @copydoc AndroidWindowBridge::OnWindowLost */
    Result<void> AndroidWindowBridge::OnWindowLost(const AndroidWindowGeneration window) {
        if (auto access = CheckAdmission(); access.HasError())
            return access;
        const OperationScope operation(operationActive_);
        for (auto &entry : entries_) {
            if (entry.reference && entry.generation == window) {
                entry.admissionClosed = true;
                if (observed_ == &entry)
                    observed_ = nullptr;
            }
        }
        return adapter_.OnWindowLost(window);
    }

    /** @copydoc AndroidWindowBridge::Drain */
    Result<std::size_t> AndroidWindowBridge::Drain() {
        if (auto access = CheckAccess(); access.HasError())
            return Result<std::size_t>::Failure(std::move(access).ErrorValue());
        const OperationScope operation(operationActive_);
        return DrainLifecycle();
    }

    Result<std::size_t> AndroidWindowBridge::DrainLifecycle() {
        auto drained = controller_.Drain();
        const auto state = controller_.Snapshot();
        if (state.HasError())
            return Result<std::size_t>::Failure(state.ErrorValue());
        observed_ = nullptr;
        for (auto &entry : entries_) {
            if (!state.Value().hasWindow || entry.activity != state.Value().activityGeneration ||
                entry.generation != state.Value().windowGeneration) {
                entry.retired = true;
                if (entry.pins == 0)
                    entry.reference.reset();
            } else if (entry.reference && !entry.admissionClosed && !closing_)
                observed_ = &entry;
        }
        return drained;
    }

    Result<void> AndroidWindowBridge::CheckWindowRequest(ANativeWindow *window) const {
        if (selection_ != AndroidWindowSelection::NoPresentation && selection_ != AndroidWindowSelection::AndroidVulkan) {
            return Result<void>::Failure(MakeError(AndroidWindowErrors::UnsupportedBackend));
        }
        if (selection_ == AndroidWindowSelection::NoPresentation) {
            return Result<void>::Failure(MakeError(AndroidWindowErrors::NoPresentation));
        }
        if (!window || !operations_.acquire || !operations_.release)
            return Result<void>::Failure(MakeError(PlatformErrors::InvalidLifecycleConfiguration));
        return Result<void>::Success();
    }

    Result<AndroidWindowBridge::NativeReference> AndroidWindowBridge::Retain(ANativeWindow *window) const {
        if (!operations_.acquire(operations_.context, window))
            return Result<NativeReference>::Failure(MakeError(AndroidWindowErrors::AcquireFailed));
        return Result<NativeReference>::Success(NativeReference(window, NativeRelease{operations_}));
    }

    /** @copydoc AndroidWindowBridge::OnWindowAvailable */
    Result<AndroidWindowGeneration> AndroidWindowBridge::OnWindowAvailable(const AndroidActivityGeneration activity,
                                                                           ANativeWindow *window) {
        if (auto access = CheckAdmission(); access.HasError())
            return Result<AndroidWindowGeneration>::Failure(std::move(access).ErrorValue());
        const OperationScope operation(operationActive_);
        if (auto selection = CheckWindowRequest(window); selection.HasError())
            return Result<AndroidWindowGeneration>::Failure(std::move(selection).ErrorValue());
        if (observed_ && observed_->activity == activity && observed_->reference.get() == window)
            return Result<AndroidWindowGeneration>::Success(observed_->generation);
        auto slot = std::ranges::find_if(entries_, [](const Entry &entry) {
            return !entry.reference;
        });
        if (slot == entries_.end())
            return Result<AndroidWindowGeneration>::Failure(MakeError(PlatformErrors::LifecycleQueueSaturated));
        auto retained = Retain(window);
        if (retained.HasError())
            return Result<AndroidWindowGeneration>::Failure(std::move(retained).ErrorValue());
        auto generation = adapter_.OnWindowAvailable(activity);
        if (generation.HasError())
            return Result<AndroidWindowGeneration>::Failure(std::move(generation).ErrorValue());
        slot->reference = std::move(retained).Value();
        slot->activity = activity;
        slot->generation = generation.Value();
        slot->retired = false;
        slot->admissionClosed = false;
        observed_ = &*slot;
        return generation;
    }
}  // namespace Horo::Platform::Android
