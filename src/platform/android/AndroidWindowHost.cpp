#include "AndroidWindowHost.h"

#include "AndroidLifecycleAdapter.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace Horo::Platform::Android {
    /** @brief Resolves the compiled target's genuine native operations without selecting a renderer. */
    AndroidWindowNativeOperations NativeAndroidWindowOperations() noexcept;

    namespace AndroidWindowErrors {
        const ErrorDomainId Domain{"horo.platform.android_window"};
        const ErrorCodeDescriptor InvalidInput{Domain, ErrorCode{"invalid_input"}, ErrorSeverity::Error,
                                               "Android window configuration or callback input is invalid."};
        const ErrorCodeDescriptor WrongThread{Domain, ErrorCode{"wrong_thread"}, ErrorSeverity::Error,
                                              "Android window operation requires its owner lane."};
        const ErrorCodeDescriptor Unsupported{Domain, ErrorCode{"unsupported"}, ErrorSeverity::Error,
                                              "Native Android window operations are unavailable on this target."};
        const ErrorCodeDescriptor Headless{Domain, ErrorCode{"headless"}, ErrorSeverity::Error,
                                           "The explicit headless profile admits no native presentation window."};
        const ErrorCodeDescriptor Closed{Domain, ErrorCode{"closed"}, ErrorSeverity::Error, "Android window admission is closed."};
        const ErrorCodeDescriptor Pending{Domain, ErrorCode{"pending"}, ErrorSeverity::Error,
                                          "Android window occurrence is not yet committed and drawable."};
        const ErrorCodeDescriptor StaleGeneration{Domain, ErrorCode{"stale_generation"}, ErrorSeverity::Error,
                                                  "Android window callback or lease names a retired occurrence."};
        const ErrorCodeDescriptor CapacityExceeded{Domain, ErrorCode{"capacity_exceeded"}, ErrorSeverity::Error,
                                                   "Bounded Android window occurrence or lease capacity is exhausted."};
        const ErrorCodeDescriptor RevisionExhausted{Domain, ErrorCode{"revision_exhausted"}, ErrorSeverity::Error,
                                                    "Android native window facts cannot advance without revision reuse."};
        const ErrorCodeDescriptor AllocationFailed{Domain, ErrorCode{"allocation_failed"}, ErrorSeverity::Error,
                                                   "Cold Android window ownership allocation failed."};
    }  // namespace AndroidWindowErrors

    namespace {
        /** @brief Preserves typed failures at the private host boundary. */
        template <class T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Recognizes a complete trusted native operation table. */
        bool CompleteOperations(const AndroidWindowNativeOperations &operations) noexcept {
            return operations.acquire && operations.release && operations.extent;
        }

        /** @brief Distinguishes default native resolution from a malformed partially supplied table. */
        bool EmptyOperations(const AndroidWindowNativeOperations &operations) noexcept {
            return !operations.context && !operations.acquire && !operations.release && !operations.extent;
        }
    }  // namespace

    /** @brief Fixed native inventory; only the existing lifecycle controller assigns Activity/window identities. */
    struct AndroidWindowHostState final {
        /** @brief One acquired native reference shared by bounded value leases for the same occurrence. */
        struct Slot final {
            void *native{};
            AndroidActivityGeneration activity;
            AndroidWindowGeneration window;
            AndroidWindowExtent extent;
            std::size_t leases{};
            bool retired{};
            bool committed{};
        };

        AndroidLifecycleController &lifecycle;
        AndroidLifecycleAdapter &adapter;
        AndroidWindowMode mode;
        AndroidWindowNativeOperations operations;
        std::thread::id owner{std::this_thread::get_id()};
        std::array<Slot, AndroidWindowHost::MaximumOccurrences> slots{};
        std::size_t current{AndroidWindowHost::MaximumOccurrences};
        std::uint64_t revision{1};
        bool closed{};

        /** @brief Enforces serialized ownership and optional presentation admission before native work. */
        Result<void> Access(bool admission = false) const {
            if (owner != std::this_thread::get_id())
                return Failure<void>(AndroidWindowErrors::WrongThread);
            if (!admission)
                return Result<void>::Success();
            if (closed)
                return Failure<void>(AndroidWindowErrors::Closed);
            if (mode == AndroidWindowMode::Headless)
                return Failure<void>(AndroidWindowErrors::Headless);
            return Result<void>::Success();
        }

        /** @brief Releases an actual reference only when no renderer lease can still borrow it. */
        void ReleaseRetired(Slot &slot) noexcept {
            if (!slot.native || !slot.retired || slot.leases != 0)
                return;
            operations.release(operations.context, slot.native);
            slot = {};
        }

        /** @brief Revokes current admission while retaining all outstanding renderer leases. */
        void RetireCurrent() noexcept {
            if (current == slots.size())
                return;
            auto &slot = slots[current];
            slot.retired = true;
            current = slots.size();
            ReleaseRetired(slot);
        }

        /** @brief Closes every occurrence without waiting or releasing borrowed native storage. */
        void RetireAll() noexcept {
            closed = true;
            current = slots.size();
            for (auto &slot : slots) {
                slot.retired = true;
                ReleaseRetired(slot);
            }
        }

        /** @brief Advances facts identity or fails closed without wrapping. */
        bool AdvanceRevision() noexcept {
            if (revision == std::numeric_limits<std::uint64_t>::max()) {
                RetireAll();
                return false;
            }
            ++revision;
            return true;
        }

        /** @brief Derives currentness from genuine committed lifecycle identities, never from callback intent. */
        bool Matches(const AndroidLifecycleSnapshot &snapshot, const Slot &slot) const noexcept {
            return !closed && !slot.retired && snapshot.hasWindow && snapshot.activityGeneration == slot.activity &&
                   snapshot.windowGeneration == slot.window;
        }

        /** @brief Copies facts while independently fencing a lifecycle loss that has not yet been refreshed. */
        AndroidWindowSnapshot Read(const AndroidLifecycleSnapshot &snapshot) const noexcept {
            AndroidWindowSnapshot result{snapshot.activityGeneration, snapshot.windowGeneration, {}, revision, false};
            if (current == slots.size())
                return result;
            const auto &slot = slots[current];
            if (!slot.committed || !Matches(snapshot, slot))
                return result;
            result.extent = slot.extent;
            result.available = true;
            return result;
        }

        /** @brief Checks whether native ownership can retire without destroying any live renderer lease. */
        bool Drained() const noexcept {
            return std::ranges::all_of(slots, [](const auto &slot) {
                return slot.leases == 0;
            });
        }

        /** @brief Rejects stale callback Activity identities before they can revoke the current window. */
        Result<void> AdmitActivity(AndroidActivityGeneration activity) const {
            if (const auto access = Access(true); access.HasError())
                return access;
            const auto actual = lifecycle.Snapshot();
            if (actual.HasError())
                return Result<void>::Failure(actual.ErrorValue());
            if (actual.Value().activityGeneration != activity || activity.value == 0 ||
                actual.Value().activity == AndroidActivityState::Destroyed)
                return Failure<void>(AndroidWindowErrors::StaleGeneration);
            return Result<void>::Success();
        }

        /** @brief Recognizes only a duplicate live native occurrence, never a retired same-pointer occurrence. */
        std::optional<AndroidWindowGeneration> Duplicate(AndroidActivityGeneration activity, void *native) const noexcept {
            if (current == slots.size())
                return {};
            const auto &slot = slots[current];
            if (slot.native != native || slot.activity != activity || slot.retired)
                return {};
            if (!slot.committed)
                return slot.window;
            const auto actual = lifecycle.Snapshot();
            if (actual.HasValue() && Matches(actual.Value(), slot))
                return slot.window;
            return {};
        }

        /** @brief Invalidates replaced admission before reserving a bounded new native occurrence. */
        Result<std::size_t> ReserveOccurrence(void *native) {
            RetireCurrent();
            if (!AdvanceRevision())
                return Failure<std::size_t>(AndroidWindowErrors::RevisionExhausted);
            if (!native)
                return Failure<std::size_t>(AndroidWindowErrors::InvalidInput);
            const auto free = std::ranges::find(slots, nullptr, &Slot::native);
            if (free == slots.end())
                return Failure<std::size_t>(AndroidWindowErrors::CapacityExceeded);
            return Result<std::size_t>::Success(static_cast<std::size_t>(free - slots.begin()));
        }

        /** @brief Publishes changed extent only at an authentic lifecycle refresh boundary. */
        Result<void> CommitExtent(Slot &slot) {
            const auto extent = operations.extent(operations.context, slot.native);
            if (slot.committed && slot.extent == extent)
                return Result<void>::Success();
            if (!AdvanceRevision())
                return Failure<void>(AndroidWindowErrors::RevisionExhausted);
            slot.extent = extent;
            slot.committed = true;
            return Result<void>::Success();
        }

        /** @brief Distinguishes a real queued candidate from a stale or foreign requested generation. */
        Result<void> RequestedGeneration(AndroidWindowGeneration window, const AndroidWindowSnapshot &snapshot) const {
            if (snapshot.window == window)
                return Result<void>::Success();
            if (current != slots.size() && slots[current].window == window)
                return Failure<void>(AndroidWindowErrors::Pending);
            return Failure<void>(AndroidWindowErrors::StaleGeneration);
        }
    };

    /** @copydoc AndroidWindowLease::AndroidWindowLease */
    AndroidWindowLease::AndroidWindowLease(AndroidWindowHostState &state, std::size_t slot, AndroidWindowSnapshot snapshot) noexcept
        : state_(&state), slot_(slot), snapshot_(snapshot) {}

    /** @copydoc AndroidWindowLease::~AndroidWindowLease */
    AndroidWindowLease::~AndroidWindowLease() {
        Reset();
    }

    /** @copydoc AndroidWindowLease::AndroidWindowLease */
    AndroidWindowLease::AndroidWindowLease(AndroidWindowLease &&other) noexcept
        : state_(std::exchange(other.state_, nullptr)), slot_(other.slot_), snapshot_(other.snapshot_) {}

    /** @copydoc AndroidWindowLease::operator= */
    AndroidWindowLease &AndroidWindowLease::operator=(AndroidWindowLease &&other) noexcept {
        if (this != &other) {
            Reset();
            state_ = std::exchange(other.state_, nullptr);
            slot_ = other.slot_;
            snapshot_ = other.snapshot_;
        }
        return *this;
    }

    /** @brief Releases only on the owning lane; a wrong-thread destructor never asynchronously frees native storage. */
    void AndroidWindowLease::Reset() noexcept {
        if (!state_)
            return;
        if (state_->owner != std::this_thread::get_id())
            std::terminate();
        auto &slot = state_->slots[slot_];
        if (!slot.native || slot.window != snapshot_.window || slot.leases == 0)
            std::terminate();
        --slot.leases;
        state_->ReleaseRetired(slot);
        state_ = nullptr;
    }

    /** @copydoc AndroidWindowLease::Snapshot */
    AndroidWindowSnapshot AndroidWindowLease::Snapshot() const noexcept {
        return state_ ? snapshot_ : AndroidWindowSnapshot{};
    }

    /** @copydoc AndroidWindowLease::NativeHandle */
    void *AndroidWindowLease::NativeHandle() const noexcept {
        return state_ && state_->owner == std::this_thread::get_id() ? state_->slots[slot_].native : nullptr;
    }

    /** @copydoc AndroidWindowLease::IsCurrent */
    bool AndroidWindowLease::IsCurrent() const noexcept {
        if (!state_ || state_->owner != std::this_thread::get_id() || state_->revision != snapshot_.revision)
            return false;
        const auto actual = state_->lifecycle.Snapshot();
        return actual.HasValue() && state_->slots[slot_].committed && state_->Matches(actual.Value(), state_->slots[slot_]);
    }

    /** @copydoc AndroidWindowHost::AndroidWindowHost */
    AndroidWindowHost::AndroidWindowHost(std::unique_ptr<AndroidWindowHostState> state) noexcept : state_(std::move(state)) {}

    /** @copydoc AndroidWindowHost::~AndroidWindowHost */
    AndroidWindowHost::~AndroidWindowHost() {
        if (state_->owner != std::this_thread::get_id())
            std::terminate();
        state_->RetireAll();
        if (!state_->Drained())
            std::terminate();
    }

    /** @copydoc AndroidWindowHost::Create */
    Result<std::unique_ptr<AndroidWindowHost>> AndroidWindowHost::Create(AndroidLifecycleController &lifecycle,
                                                                         AndroidLifecycleAdapter &adapter, AndroidWindowMode mode,
                                                                         AndroidWindowNativeOperations operations) {
        if (!adapter.IsBoundTo(lifecycle) || mode > AndroidWindowMode::Interactive)
            return Failure<std::unique_ptr<AndroidWindowHost>>(AndroidWindowErrors::InvalidInput);
        const auto snapshot = lifecycle.Snapshot();
        if (snapshot.HasError())
            return Result<std::unique_ptr<AndroidWindowHost>>::Failure(snapshot.ErrorValue());
        if (!EmptyOperations(operations) && !CompleteOperations(operations))
            return Failure<std::unique_ptr<AndroidWindowHost>>(AndroidWindowErrors::InvalidInput);
        if (EmptyOperations(operations))
            operations = NativeAndroidWindowOperations();
        if (mode == AndroidWindowMode::Interactive && !CompleteOperations(operations))
            return Failure<std::unique_ptr<AndroidWindowHost>>(AndroidWindowErrors::Unsupported);
        try {
            auto state = std::make_unique<AndroidWindowHostState>(lifecycle, adapter, mode, operations);
            return Result<std::unique_ptr<AndroidWindowHost>>::Success(
                std::unique_ptr<AndroidWindowHost>(new AndroidWindowHost(std::move(state))));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<AndroidWindowHost>>(AndroidWindowErrors::AllocationFailed);
        }
    }

    /** @copydoc AndroidWindowHost::ObserveAvailable */
    Result<AndroidWindowGeneration> AndroidWindowHost::ObserveAvailable(AndroidActivityGeneration activity, void *nativeWindow) {
        if (const auto admitted = state_->AdmitActivity(activity); admitted.HasError())
            return Result<AndroidWindowGeneration>::Failure(admitted.ErrorValue());
        if (const auto duplicate = state_->Duplicate(activity, nativeWindow))
            return Result<AndroidWindowGeneration>::Success(*duplicate);
        const auto reserved = state_->ReserveOccurrence(nativeWindow);
        if (reserved.HasError())
            return Result<AndroidWindowGeneration>::Failure(reserved.ErrorValue());
        const auto issued = state_->adapter.OnWindowAvailable(activity);
        if (issued.HasError())
            return Result<AndroidWindowGeneration>::Failure(issued.ErrorValue());
        state_->operations.acquire(state_->operations.context, nativeWindow);
        state_->slots[reserved.Value()] = {nativeWindow, activity, issued.Value(), {}, 0, false, false};
        state_->current = reserved.Value();
        return issued;
    }

    /** @copydoc AndroidWindowHost::ObserveLost */
    Result<void> AndroidWindowHost::ObserveLost(AndroidWindowGeneration window) {
        if (const auto access = state_->Access(true); access.HasError())
            return access;
        if (state_->current == state_->slots.size() || state_->slots[state_->current].window != window)
            return Failure<void>(AndroidWindowErrors::StaleGeneration);
        state_->RetireCurrent();
        if (!state_->AdvanceRevision())
            return Failure<void>(AndroidWindowErrors::RevisionExhausted);
        return state_->adapter.OnWindowLost(window);
    }

    /** @copydoc AndroidWindowHost::Refresh */
    Result<void> AndroidWindowHost::Refresh() {
        if (const auto access = state_->Access(); access.HasError())
            return access;
        const auto actual = state_->lifecycle.Snapshot();
        if (actual.HasError())
            return Result<void>::Failure(actual.ErrorValue());
        if (actual.Value().queuedObservationCount != 0)
            return Failure<void>(AndroidWindowErrors::Pending);
        if (state_->closed || state_->current == state_->slots.size())
            return Result<void>::Success();
        auto &slot = state_->slots[state_->current];
        if (!state_->Matches(actual.Value(), slot)) {
            state_->RetireCurrent();
            return state_->AdvanceRevision() ? Result<void>::Success() : Failure<void>(AndroidWindowErrors::RevisionExhausted);
        }
        return state_->CommitExtent(slot);
    }

    /** @copydoc AndroidWindowHost::Snapshot */
    Result<AndroidWindowSnapshot> AndroidWindowHost::Snapshot() const {
        if (const auto access = state_->Access(); access.HasError())
            return Result<AndroidWindowSnapshot>::Failure(access.ErrorValue());
        const auto actual = state_->lifecycle.Snapshot();
        if (actual.HasError())
            return Result<AndroidWindowSnapshot>::Failure(actual.ErrorValue());
        return Result<AndroidWindowSnapshot>::Success(state_->Read(actual.Value()));
    }

    /** @copydoc AndroidWindowHost::Acquire */
    Result<AndroidWindowLease> AndroidWindowHost::Acquire(AndroidWindowGeneration window) {
        if (const auto access = state_->Access(true); access.HasError())
            return Result<AndroidWindowLease>::Failure(access.ErrorValue());
        const auto snapshot = Snapshot();
        if (snapshot.HasError())
            return Result<AndroidWindowLease>::Failure(snapshot.ErrorValue());
        if (const auto requested = state_->RequestedGeneration(window, snapshot.Value()); requested.HasError())
            return Result<AndroidWindowLease>::Failure(requested.ErrorValue());
        if (!snapshot.Value().available || snapshot.Value().extent.width == 0 || snapshot.Value().extent.height == 0)
            return Failure<AndroidWindowLease>(AndroidWindowErrors::Pending);
        auto &slot = state_->slots[state_->current];
        if (slot.leases == MaximumLeasesPerOccurrence)
            return Failure<AndroidWindowLease>(AndroidWindowErrors::CapacityExceeded);
        ++slot.leases;
        return Result<AndroidWindowLease>::Success(AndroidWindowLease{*state_, state_->current, snapshot.Value()});
    }

    /** @copydoc AndroidWindowHost::Close */
    Result<bool> AndroidWindowHost::Close() {
        if (const auto access = state_->Access(); access.HasError())
            return Result<bool>::Failure(access.ErrorValue());
        if (!state_->closed) {
            state_->RetireAll();
            if (!state_->AdvanceRevision())
                return Failure<bool>(AndroidWindowErrors::RevisionExhausted);
        }
        return Result<bool>::Success(state_->Drained());
    }
}  // namespace Horo::Platform::Android
