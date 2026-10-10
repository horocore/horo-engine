#include "Horo/Runtime/Ui/UiScreenTransition.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace UiScreenTransitionErrors {
        /** @copydoc Invalid */
        const ErrorCodeDescriptor Invalid{ErrorDomainId{"horo.runtime_ui"},
                                          ErrorCode{"runtime_ui.screen_transition.invalid"},
                                          ErrorSeverity::Error,
                                          "Invalid screen transition request or clock.",
                                          "Supply fresh owner identities and a nondecreasing clock with a positive timeout.",
                                          false,
                                          false};
        /** @copydoc Busy */
        const ErrorCodeDescriptor Busy{ErrorDomainId{"horo.runtime_ui"},
                                       ErrorCode{"runtime_ui.screen_transition.busy"},
                                       ErrorSeverity::Error,
                                       "Screen transition storage is still retained.",
                                       "Collect terminal work and drained screen leases before retrying.",
                                       true,
                                       false};
        /** @copydoc Timeout */
        const ErrorCodeDescriptor Timeout{ErrorDomainId{"horo.runtime_ui"},
                                          ErrorCode{"runtime_ui.screen_transition.timeout"},
                                          ErrorSeverity::Error,
                                          "Screen preparation exceeded its deadline.",
                                          "Collect cancelled work and retry while retaining the last-good screen.",
                                          true,
                                          false};
    }  // namespace UiScreenTransitionErrors

    namespace {
        template <typename T> Result<T> Fail(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Identifies stages that can still reach publication. */
        bool Pending(const UiScreenTransitionState state) noexcept {
            using enum UiScreenTransitionState;
            return state == LoadingDocument || state == LoadingDependencies || state == LoadCompleted || state == AwaitingComposition ||
                   state == Ready;
        }
    }  // namespace

    struct UiScreenTransition::Storage final {
        /** @brief Owns immutable terminal errors independently from current screen lifetime. */
        struct FailureState final {
            std::optional<Error> detail;
            Error invalid{MakeError(UiScreenTransitionErrors::Invalid)};
            Error timeout{MakeError(UiScreenTransitionErrors::Timeout)};
            Error cancelled{MakeError(UiErrors::AssetLoadCancelled)};
        };

        /** @brief Tracks the host's monotonic preparation clock. */
        struct Timing final {
            std::uint64_t started{};
            std::uint64_t lastTick{};
        };

        Storage(UiRuntimeAssetLoadService &service, UiOwnershipGeneration owner, UiScreenTransitionLimits bounds)
            : loader(service), ownership(owner), limits(bounds), retired(bounds.maximumRetiredScreens) {}

        /** @brief Preserves the original failure while leaving live screen ownership untouched. */
        Result<void> Reject(Error error) {
            failures.detail = std::move(error);
            progress.state = UiScreenTransitionState::Failed;
            return Result<void>::Failure(*failures.detail);
        }

        /** @brief Reads the current exact route guard without changing route ownership. */
        Result<UiRouteStackGuard> Guard() const {
            if (!current || !current->Current())
                return Fail<UiRouteStackGuard>(UiErrors::RouteOperationLifecycleUnavailable);
            auto *canvas = current->Current()->Canvas(currentCanvas);
            if (!canvas || !canvas->routes)
                return Fail<UiRouteStackGuard>(UiErrors::RouteStackInvalid);
            return canvas->routes->Guard();
        }

        /** @brief Verifies the exact source generation and navigation guard without allocation or mutation. */
        bool SourceMatches() const noexcept {
            if (!source || !current->IsCurrent(*source))
                return false;
            auto *canvas = current->Current()->Canvas(currentCanvas);
            if (!canvas || !canvas->routes || !sourceGuard || canvas->routes->State() != UiScreenStackState::Active)
                return false;
            const auto top = canvas->routes->Top();
            return canvas->routes->Stack() == sourceGuard->stack && canvas->routes->Revision() == sourceGuard->revision &&
                   (top ? std::optional{top->id} : std::nullopt) == sourceGuard->top;
        }

        /** @brief Refuses retirement while any canvas owns a prepared navigation mutation. */
        bool RoutesCanRetire() const noexcept {
            for (const auto &canvas : current->Current()->Canvases())
                if (canvas.routes && !canvas.routes->CanRetire())
                    return false;
            return true;
        }

        /** @brief Releases terminal preparation resources only while the collection fence is held. */
        Result<void> CollectPreparation() {
            if (Pending(progress.state))
                return Result<void>::Success();
            if (loading) {
                (void)loading->RequestCancel();
                loading.reset();
            }
            if (candidate) {
                candidate->Shutdown();
                if (auto collected = candidate->CollectRetired(); collected.HasError())
                    return Result<void>::Failure(collected.ErrorValue());
                if (!candidate->CanReclaim())
                    return Fail<void>(UiScreenTransitionErrors::Busy);
                candidate.reset();
            }
            source.reset();
            loaded.reset();
            sourceGuard.reset();
            request.reset();
            cancellation = {};
            return Result<void>::Success();
        }

        /** @brief Collects publisher generations and destroys only drained retired or stopped owners. */
        Result<std::size_t> CollectPublishers() {
            std::size_t count{};
            for (auto &publisher : retired) {
                if (!publisher)
                    continue;
                if (auto collected = publisher->CollectRetired(); collected.HasError())
                    return Result<std::size_t>::Failure(collected.ErrorValue());
                if (publisher->CanReclaim()) {
                    publisher.reset();
                    ++count;
                }
            }
            if (current) {
                if (auto collected = current->CollectRetired(); collected.HasError())
                    return Result<std::size_t>::Failure(collected.ErrorValue());
                if (progress.state == UiScreenTransitionState::Stopped && current->CanReclaim()) {
                    current.reset();
                    ++count;
                }
            }
            return Result<std::size_t>::Success(count);
        }

        UiRuntimeAssetLoadService &loader;
        UiOwnershipGeneration ownership;
        UiScreenTransitionLimits limits;
        std::unique_ptr<UiHotReload> current;
        std::unique_ptr<UiHotReload> candidate;
        std::vector<std::unique_ptr<UiHotReload>> retired;
        std::optional<UiRuntimeAssetLoadHandle> loading;
        std::optional<UiRuntimeAssetLoadResult> loaded;
        std::optional<UiScreenTransitionRequest> request;
        CancellationToken cancellation;
        std::optional<UiReloadLease> source;
        std::optional<UiRouteStackGuard> sourceGuard;
        UiCanvasId currentCanvas;
        UiScreenTransitionProgress progress;
        Timing timing;
        std::uint64_t nextOperation{1};
        std::uint32_t lastInstanceSlot{};
        FailureState failures;
        bool collecting{}; /**< Closes public borrowing and mutation while deferred authority callbacks drain. */
        bool stopRequested{};
    };

    /** @copydoc UiScreenTransition::Create */
    Result<UiScreenTransition> UiScreenTransition::Create(UiRuntimeAssetLoadService &loader, const UiOwnershipGeneration ownership,
                                                          const UiScreenTransitionLimits limits) {
        if (!ownership.IsValid() || limits.maximumRetiredScreens == 0 || limits.maximumRetiredScreens > 64 || !limits.reload.IsValid())
            return Fail<UiScreenTransition>(UiScreenTransitionErrors::Invalid);
        try {
            return Result<UiScreenTransition>::Success(UiScreenTransition{std::make_unique<Storage>(loader, ownership, limits)});
        } catch (const std::bad_alloc &) {
            return Fail<UiScreenTransition>(UiErrors::AssetBudgetExceeded);
        }
    }

    /** @copydoc UiScreenTransition::UiScreenTransition */
    UiScreenTransition::UiScreenTransition(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiScreenTransition::~UiScreenTransition */
    UiScreenTransition::~UiScreenTransition() {
        Shutdown();
        if (storage_ && storage_->loading)
            (void)storage_->loading->RequestCancel();
    }

    /** @copydoc UiScreenTransition::UiScreenTransition */
    UiScreenTransition::UiScreenTransition(UiScreenTransition &&) noexcept = default;

    /** @copydoc UiScreenTransition::operator= */
    UiScreenTransition &UiScreenTransition::operator=(UiScreenTransition &&other) noexcept {
        if (this != &other) {
            Shutdown();
            if (storage_ && storage_->loading)
                (void)storage_->loading->RequestCancel();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiScreenTransition::Begin */
    Result<UiScreenTransitionId> UiScreenTransition::Begin(UiScreenTransitionRequest request, const std::uint64_t now,
                                                           const CancellationToken &cancellation) {
        if (!storage_ || storage_->progress.state == UiScreenTransitionState::Stopped)
            return Fail<UiScreenTransitionId>(UiErrors::AssetLoadShutdown);
        auto &s = *storage_;
        if (s.collecting || s.request || s.loading || s.candidate)
            return Fail<UiScreenTransitionId>(UiScreenTransitionErrors::Busy);
        if (!request.instance.IsValid() || request.instance.ownership != s.ownership || request.instance.slot <= s.lastInstanceSlot ||
            !request.route.IsValid() || request.timeoutTicks == 0 || now < s.timing.lastTick)
            return Fail<UiScreenTransitionId>(UiScreenTransitionErrors::Invalid);
        if (s.nextOperation == 0)
            return Fail<UiScreenTransitionId>(UiErrors::GenerationExhausted);
        std::optional<UiReloadLease> source;
        std::optional<UiRouteStackGuard> guard;
        if (s.current) {
            auto captured = s.current->Acquire();
            if (captured.HasError())
                return Result<UiScreenTransitionId>::Failure(captured.ErrorValue());
            source.emplace(std::move(captured).Value());
            auto observed = s.Guard();
            if (observed.HasError())
                return Result<UiScreenTransitionId>::Failure(observed.ErrorValue());
            guard = observed.Value();
        }
        auto load = s.loader.LoadAsync(request.asset, cancellation);
        if (load.HasError())
            return Result<UiScreenTransitionId>::Failure(load.ErrorValue());
        s.source = std::move(source);
        s.sourceGuard = guard;
        s.loading.emplace(std::move(load).Value());
        s.lastInstanceSlot = request.instance.slot;
        s.request.emplace(std::move(request));
        s.cancellation = cancellation;
        s.timing.started = now;
        s.timing.lastTick = now;
        s.failures.detail.reset();
        s.progress = {{s.ownership, s.nextOperation}, UiScreenTransitionState::LoadingDocument, 0, s.request->timeoutTicks};
        s.nextOperation = s.nextOperation == std::numeric_limits<std::uint64_t>::max() ? 0 : s.nextOperation + 1;
        return Result<UiScreenTransitionId>::Success(s.progress.operation);
    }

    /** @copydoc UiScreenTransition::Poll */
    UiScreenTransitionProgress UiScreenTransition::Poll(const std::uint64_t now) noexcept {
        using enum UiScreenTransitionState;
        if (!storage_)
            return {{}, Stopped, 0, 0};
        auto &s = *storage_;
        if (s.collecting || !Pending(s.progress.state))
            return s.progress;
        if (now < s.timing.lastTick) {
            s.progress.state = Failed;
            return s.progress;
        }
        s.timing.lastTick = now;
        s.progress.elapsedTicks = now - s.timing.started;
        if (s.cancellation.IsCancellationRequested()) {
            s.progress.state = Cancelled;
        } else if (s.progress.elapsedTicks >= s.progress.timeoutTicks) {
            s.progress.state = TimedOut;
        } else if (s.progress.state != Ready && !s.loaded && s.loading) {
            switch (s.loading->State()) {
                case UiRuntimeAssetLoadState::LoadingDependencies:
                    s.progress.state = LoadingDependencies;
                    break;
                case UiRuntimeAssetLoadState::Succeeded:
                    s.progress.state = LoadCompleted;
                    break;
                case UiRuntimeAssetLoadState::Cancelled:
                    s.progress.state = Cancelled;
                    break;
                case UiRuntimeAssetLoadState::Failed:
                    // Consume original failure detail only in the explicit load-time Prepare operation.
                    s.progress.state = LoadCompleted;
                    break;
                default:
                    break;
            }
        }
        return s.progress;
    }

    /** @copydoc UiScreenTransition::PrepareAssets */
    Result<void> UiScreenTransition::PrepareAssets(const UiScreenTransitionId operation, const std::uint64_t now) {
        using enum UiScreenTransitionState;
        if (!storage_ || storage_->collecting || operation != storage_->progress.operation)
            return Fail<void>(UiErrors::RevisionStale);
        if (const auto progress = Poll(now); progress.state != LoadCompleted && progress.state != AwaitingComposition)
            return Failure() ? Result<void>::Failure(*Failure()) : Fail<void>(UiErrors::AssetLoadNotReady);
        auto &s = *storage_;
        if (s.loaded)
            return Result<void>::Success();
        auto loaded = s.loading->TakeResult();
        if (loaded.HasError())
            return s.Reject(loaded.ErrorValue());
        s.loaded.emplace(std::move(loaded).Value());
        s.progress.state = AwaitingComposition;
        return Result<void>::Success();
    }

    /** @copydoc UiScreenTransition::LoadedDocument */
    const CookedUiDocument *UiScreenTransition::LoadedDocument() const noexcept {
        return storage_ && !storage_->collecting && storage_->loaded ? &storage_->loaded->document : nullptr;
    }

    /** @copydoc UiScreenTransition::Prepare */
    Result<void> UiScreenTransition::Prepare(const UiScreenTransitionId operation, std::vector<UiReloadCanvas> canvases,
                                             const std::uint64_t now) {
        if (const auto assets = PrepareAssets(operation, now); assets.HasError())
            return assets;
        auto &s = *storage_;
        auto generation = UiReloadGeneration::Create(std::move(*s.loaded), s.request->instance, std::move(canvases));
        s.loaded.reset();
        if (generation.HasError())
            return s.Reject(generation.ErrorValue());
        auto preparedGeneration = std::move(generation).Value();
        if (auto *canvas = preparedGeneration.Canvas(s.request->asset.canvas.canvas);
            !canvas || !canvas->routes || !canvas->routes->Empty())
            return s.Reject(MakeError(UiErrors::RouteStackInvalid));
        auto publisher = UiHotReload::Create(std::move(preparedGeneration), s.limits.reload);
        if (publisher.HasError())
            return s.Reject(publisher.ErrorValue());
        try {
            s.candidate = std::make_unique<UiHotReload>(std::move(publisher).Value());
        } catch (const std::bad_alloc &) {
            return s.Reject(MakeError(UiErrors::AssetBudgetExceeded));
        }
        const auto routed = s.candidate->Current()->Canvas(s.request->asset.canvas.canvas)->routes->Push(s.request->route);
        if (routed.HasError())
            return s.Reject(routed.ErrorValue());
        if (!routed.Value().IsCommitted())
            return s.Reject(MakeError(UiErrors::RouteOperationInvalid));
        s.progress.state = UiScreenTransitionState::Ready;
        return Result<void>::Success();
    }

    /** @copydoc UiScreenTransition::Commit */
    UiScreenTransitionCommitResult UiScreenTransition::Commit(const UiScreenTransitionId operation, const UiStructuralCommitPoint point,
                                                              const std::uint64_t now) noexcept {
        if (!storage_)
            return UiScreenTransitionCommitResult::Stopped;
        if (storage_->collecting)
            return UiScreenTransitionCommitResult::Collecting;
        if (operation != storage_->progress.operation)
            return UiScreenTransitionCommitResult::SourceStale;
        const auto progress = Poll(now);
        if (progress.state == UiScreenTransitionState::Stopped)
            return UiScreenTransitionCommitResult::Stopped;
        if (progress.state != UiScreenTransitionState::Ready)
            return UiScreenTransitionCommitResult::NotReady;
        auto &s = *storage_;
        if (point != UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands &&
            point != UiStructuralCommitPoint::CommitDeferredLifecycleChanges)
            return UiScreenTransitionCommitResult::InvalidPoint;
        auto slot = std::ranges::find_if(s.retired, [](const auto &publisher) {
            return !publisher;
        });
        if (s.current) {
            if (!s.SourceMatches())
                return UiScreenTransitionCommitResult::SourceStale;
            if (!s.RoutesCanRetire())
                return UiScreenTransitionCommitResult::RouteBusy;
            if (slot == s.retired.end())
                return UiScreenTransitionCommitResult::RetentionFull;
            s.current->Shutdown();
            *slot = std::move(s.current);
        }
        s.current = std::move(s.candidate);
        s.currentCanvas = s.request->asset.canvas.canvas;
        s.progress.state = UiScreenTransitionState::Committed;
        return UiScreenTransitionCommitResult::Committed;
    }

    /** @copydoc UiScreenTransition::Cancel */
    Result<void> UiScreenTransition::Cancel(const UiScreenTransitionId operation) {
        using enum UiScreenTransitionState;
        if (!storage_ || storage_->progress.state == Stopped)
            return Fail<void>(UiErrors::AssetLoadShutdown);
        auto &s = *storage_;
        if (s.collecting)
            return Fail<void>(UiScreenTransitionErrors::Busy);
        if (operation != s.progress.operation)
            return Fail<void>(UiErrors::RevisionStale);
        if (Pending(s.progress.state))
            s.progress.state = Cancelled;
        if (s.loading && s.progress.state != Committed)
            return s.loading->RequestCancel();
        return Result<void>::Success();
    }

    /** @copydoc UiScreenTransition::Current */
    UiHotReload *UiScreenTransition::Current() noexcept {
        return storage_ && !storage_->collecting && storage_->progress.state != UiScreenTransitionState::Stopped ? storage_->current.get()
                                                                                                                 : nullptr;
    }

    /** @copydoc UiScreenTransition::Acquire */
    Result<UiReloadLease> UiScreenTransition::Acquire() const {
        if (!storage_ || storage_->collecting || !storage_->current || storage_->progress.state == UiScreenTransitionState::Stopped)
            return Fail<UiReloadLease>(UiErrors::InstanceStateInvalid);
        return storage_->current->Acquire();
    }

    /** @copydoc UiScreenTransition::IsCurrent */
    bool UiScreenTransition::IsCurrent(const UiReloadLease &lease) const noexcept {
        return storage_ && !storage_->collecting && storage_->progress.state != UiScreenTransitionState::Stopped && storage_->current &&
               storage_->current->IsCurrent(lease);
    }

    /** @copydoc UiScreenTransition::Progress */
    UiScreenTransitionProgress UiScreenTransition::Progress() const noexcept {
        return storage_ ? storage_->progress : UiScreenTransitionProgress{{}, UiScreenTransitionState::Stopped, 0, 0};
    }

    /** @copydoc UiScreenTransition::Failure */
    const Error *UiScreenTransition::Failure() const noexcept {
        using enum UiScreenTransitionState;
        if (!storage_)
            return nullptr;
        const auto &s = *storage_;
        if (s.progress.state == TimedOut)
            return &s.failures.timeout;
        if (s.progress.state == Cancelled)
            return &s.failures.cancelled;
        if (s.progress.state == Failed)
            return s.failures.detail ? &*s.failures.detail : &s.failures.invalid;
        return nullptr;
    }

    /** @copydoc UiScreenTransition::CollectRetired */
    Result<std::size_t> UiScreenTransition::CollectRetired() {
        if (!storage_)
            return Result<std::size_t>::Success(0);
        auto &s = *storage_;
        if (s.collecting)
            return Fail<std::size_t>(UiScreenTransitionErrors::Busy);
        s.collecting = true;

        struct CollectionGuard final {
            UiScreenTransition &owner;
            Storage &storage;

            CollectionGuard(UiScreenTransition &ownerRef, Storage &storageRef) noexcept : owner(ownerRef), storage(storageRef) {}

            CollectionGuard(const CollectionGuard &) = delete;
            CollectionGuard &operator=(const CollectionGuard &) = delete;
            CollectionGuard(CollectionGuard &&) = delete;
            CollectionGuard &operator=(CollectionGuard &&) = delete;

            ~CollectionGuard() noexcept {
                storage.collecting = false;
                if (storage.stopRequested)
                    owner.Shutdown();
            }
        };

        CollectionGuard guard{*this, s};

        if (const auto preparation = s.CollectPreparation(); preparation.HasError())
            return Result<std::size_t>::Failure(preparation.ErrorValue());
        return s.CollectPublishers();
    }

    /** @copydoc UiScreenTransition::Shutdown */
    void UiScreenTransition::Shutdown() noexcept {
        if (!storage_ || storage_->progress.state == UiScreenTransitionState::Stopped)
            return;
        auto &s = *storage_;
        if (s.collecting) {
            s.stopRequested = true;
            return;
        }
        s.stopRequested = false;
        s.progress.state = UiScreenTransitionState::Stopped;
        if (s.current)
            s.current->Shutdown();
        if (s.candidate)
            s.candidate->Shutdown();
    }

    /** @copydoc UiScreenTransition::CanReclaim */
    bool UiScreenTransition::CanReclaim() const noexcept {
        return !storage_ || (!storage_->collecting && storage_->progress.state == UiScreenTransitionState::Stopped && !storage_->current &&
                             !storage_->candidate && !storage_->request && !storage_->loading &&
                             std::ranges::none_of(storage_->retired, [](const auto &publisher) {
            return static_cast<bool>(publisher);
        }));
    }
}  // namespace Horo::Runtime::Ui
