#include "Horo/Runtime/Ui/UiScreenStack.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsSameTop(const std::optional<UiRouteInstanceId> &expected,
                                     const std::vector<UiRouteInstance> &routes) noexcept {
            if (!expected.has_value())
                return routes.empty();
            return !routes.empty() && routes.back().id == *expected;
        }
    }  // namespace

    struct UiScreenStack::Storage final {
        explicit Storage(const UiScreenStackDescriptor &descriptor)
            : ownership(descriptor.ownership), stack(descriptor.stack),
              definitions(descriptor.definitions.begin(), descriptor.definitions.end()), maximumRoutes(descriptor.maximumRoutes),
              maximumRetiredActions(descriptor.maximumRetiredActionRouters == 0 ? descriptor.maximumRoutes
                                                                                : descriptor.maximumRetiredActionRouters),
              lastRouteIncarnation(descriptor.previousRouteIncarnation) {
            routes.reserve(maximumRoutes);
            actions.reserve(maximumRoutes);
            retiredActions.reserve(maximumRetiredActions);
        }

        [[nodiscard]] std::optional<UiRouteMetadata> Find(const UiRouteId route) const noexcept {
            const auto found = std::find_if(definitions.begin(), definitions.end(), [route](const UiRouteMetadata &candidate) {
                return candidate.id == route;
            });
            if (found == definitions.end())
                return std::nullopt;
            return *found;
        }

        [[nodiscard]] Result<UiRouteOperationId> NextOperation() {
            if (nextOperationSequence == 0)
                return Failure<UiRouteOperationId>(UiErrors::GenerationExhausted);
            const auto sequence = UiRouteOperationSequence::Create(nextOperationSequence);
            if (sequence.HasError())
                return Result<UiRouteOperationId>::Failure(sequence.ErrorValue());
            if (nextOperationSequence == std::numeric_limits<std::uint64_t>::max())
                nextOperationSequence = 0;
            else
                ++nextOperationSequence;
            return Result<UiRouteOperationId>::Success({ownership, sequence.Value()});
        }

        [[nodiscard]] Result<UiRouteInstanceId> NextInstance() {
            if (lastRouteIncarnation == std::numeric_limits<std::uint32_t>::max())
                return Failure<UiRouteInstanceId>(UiErrors::GenerationExhausted);
            const UiRouteInstanceId instance{ownership, stack.slot, ++lastRouteIncarnation};
            return Result<UiRouteInstanceId>::Success(instance);
        }

        UiOwnershipGeneration ownership;
        UiRouteStackId stack;
        std::vector<UiRouteMetadata> definitions;
        std::vector<UiRouteInstance> routes;

        struct RouteActions final {
            UiRouteInstanceId route;
            UiActionRouter router;
        };

        std::vector<RouteActions> actions;
        std::vector<RouteActions> retiredActions;

        /** @brief Preflights exact removal storage while the active operation excludes competing attach/navigation. */
        [[nodiscard]] bool CanRetireFor(const UiRouteOperationKind kind) const noexcept {
            std::size_t required{};
            if (kind == UiRouteOperationKind::Clear)
                required = actions.size();
            else if ((kind == UiRouteOperationKind::Pop || kind == UiRouteOperationKind::Back || kind == UiRouteOperationKind::Replace) &&
                     !routes.empty())
                required = std::ranges::find(actions, routes.back().id, &RouteActions::route) != actions.end() ? 1U : 0U;
            return required <= maximumRetiredActions - retiredActions.size();
        }

        /** @brief Cancels before releasing the exact route-owned router; no widget callback is invoked. */
        void RetireActions(const UiRouteInstanceId route, const UiActionCancellationReason reason) noexcept {
            const auto found = std::ranges::find(actions, route, &RouteActions::route);
            if (found != actions.end()) {
                if (found->router.State() == UiActionRouterState::Active)
                    (void)found->router.BeginRetirement(reason);
                retiredActions.push_back({found->route, std::move(found->router)});
                // The erased destination and each shifted source are moved-from routers, so erase cannot reclaim router storage.
                actions.erase(found);
            }
        }

        std::size_t maximumRoutes;
        std::size_t maximumRetiredActions;
        UiRouteStackRevision revision{UiRouteStackRevision::Create(1).Value()};
        std::uint32_t lastRouteIncarnation{};
        std::uint64_t nextOperationSequence{1};
        UiRouteOperationId activeOperation;
        UiScreenStackState state{UiScreenStackState::Active};
        bool busy{};
    };

    /** @copydoc UiScreenStack::Transaction::Transaction */
    UiScreenStack::Transaction::Transaction(std::shared_ptr<Storage> storage, const UiRouteOperationRequest request,
                                            const UiRouteOperationId operation, const std::optional<UiRouteMetadata> definition,
                                            const UiRouteOperationRejection preparedRejection) noexcept
        : storage_(std::move(storage)), request_(request), operation_(operation), definition_(definition),
          preparedRejection_(preparedRejection) {}

    /** @copydoc UiScreenStack::Transaction::~Transaction */
    UiScreenStack::Transaction::~Transaction() {
        Abandon();
    }

    /** @copydoc UiScreenStack::Transaction::Transaction */
    UiScreenStack::Transaction::Transaction(Transaction &&other) noexcept
        : storage_(std::move(other.storage_)), request_(other.request_), operation_(other.operation_), definition_(other.definition_),
          preparedRejection_(other.preparedRejection_), terminal_(other.terminal_) {
        other.terminal_ = true;
    }

    /** @copydoc UiScreenStack::Transaction::operator= */
    UiScreenStack::Transaction &UiScreenStack::Transaction::operator=(Transaction &&other) noexcept {
        if (this == &other)
            return *this;
        Abandon();
        storage_ = std::move(other.storage_);
        request_ = other.request_;
        operation_ = other.operation_;
        definition_ = other.definition_;
        preparedRejection_ = other.preparedRejection_;
        terminal_ = other.terminal_;
        other.terminal_ = true;
        return *this;
    }

    /** @copydoc UiScreenStack::Transaction::Commit */
    Result<UiRouteOperationResult> UiScreenStack::Transaction::Commit() {
        if (terminal_ || storage_ == nullptr)
            return Failure<UiRouteOperationResult>(UiErrors::RouteOperationAlreadyCompleted);
        return UiScreenStack::Commit(*this);
    }

    /** @copydoc UiScreenStack::Transaction::Cancel */
    Result<UiRouteOperationResult> UiScreenStack::Transaction::Cancel() {
        if (terminal_ || storage_ == nullptr)
            return Failure<UiRouteOperationResult>(UiErrors::RouteOperationAlreadyCompleted);
        return UiScreenStack::Cancel(*this);
    }

    /** @copydoc UiScreenStack::Transaction::Operation */
    UiRouteOperationId UiScreenStack::Transaction::Operation() const noexcept {
        return operation_;
    }

    /** @copydoc UiScreenStack::Transaction::Abandon */
    void UiScreenStack::Transaction::Abandon() noexcept {
        if (!terminal_ && storage_ != nullptr)
            UiScreenStack::Abandon(*this);
        storage_ = nullptr;
        terminal_ = true;
    }

    /** @copydoc UiScreenStack::Create */
    Result<UiScreenStack> UiScreenStack::Create(const UiScreenStackDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiScreenStack>(UiErrors::RouteStackInvalid);
        try {
            return Result<UiScreenStack>::Success(UiScreenStack{std::make_shared<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiScreenStack>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiScreenStack::UiScreenStack */
    UiScreenStack::UiScreenStack(std::shared_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiScreenStack::~UiScreenStack */
    UiScreenStack::~UiScreenStack() {
        Shutdown();
    }

    /** @copydoc UiScreenStack::UiScreenStack */
    UiScreenStack::UiScreenStack(UiScreenStack &&) noexcept = default;

    /** @copydoc UiScreenStack::operator= */
    UiScreenStack &UiScreenStack::operator=(UiScreenStack &&other) noexcept {
        if (this != &other) {
            Shutdown();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiScreenStack::ApplyIssuedMutation */
    std::optional<UiRouteInstanceId> UiScreenStack::ApplyIssuedMutation(Storage &storage, const Transaction &transaction,
                                                                        const std::optional<UiRouteInstanceId> instance) noexcept {
        switch (transaction.request_.kind) {
            case UiRouteOperationKind::Push:
            case UiRouteOperationKind::Navigate:
                if (!storage.routes.empty())
                    storage.routes.back().visibility = UiRouteVisibilityState::Covered;
                storage.routes.push_back({*instance, *transaction.definition_, UiRouteVisibilityState::Visible});
                return instance;
            case UiRouteOperationKind::Pop:
            case UiRouteOperationKind::Back:
                storage.RetireActions(storage.routes.back().id, UiActionCancellationReason::OwnerRetired);
                storage.routes.pop_back();
                if (storage.routes.empty())
                    return std::nullopt;
                storage.routes.back().visibility = UiRouteVisibilityState::Visible;
                return storage.routes.back().id;
            case UiRouteOperationKind::Replace:
                storage.RetireActions(storage.routes.back().id, UiActionCancellationReason::Superseded);
                storage.routes.back() = {*instance, *transaction.definition_, UiRouteVisibilityState::Visible};
                return instance;
            case UiRouteOperationKind::Clear:
                for (const auto &route : storage.routes)
                    storage.RetireActions(route.id, UiActionCancellationReason::OwnerRetired);
                storage.routes.clear();
                return std::nullopt;
            case UiRouteOperationKind::Count:
                return std::nullopt;
        }
        return std::nullopt;
    }

    /** @copydoc UiScreenStack::ApplyMutation */
    Result<std::optional<UiRouteInstanceId>> UiScreenStack::ApplyMutation(Storage &storage, Transaction &transaction) {
        std::optional<UiRouteInstanceId> instance;
        if (transaction.request_.kind == UiRouteOperationKind::Push || transaction.request_.kind == UiRouteOperationKind::Navigate ||
            transaction.request_.kind == UiRouteOperationKind::Replace) {
            const auto issued = storage.NextInstance();
            if (issued.HasError())
                return Result<std::optional<UiRouteInstanceId>>::Failure(issued.ErrorValue());
            instance = issued.Value();
        }
        return Result<std::optional<UiRouteInstanceId>>::Success(ApplyIssuedMutation(storage, transaction, instance));
    }

    /** @copydoc UiScreenStack::Finish */
    void UiScreenStack::Finish(Transaction &transaction) noexcept {
        transaction.storage_->busy = false;
        transaction.storage_->activeOperation = {};
        transaction.terminal_ = true;
        transaction.storage_ = nullptr;
    }

    /** @copydoc UiScreenStack::AnimationGate::AnimationGate */
    UiScreenStack::AnimationGate::AnimationGate(Transaction transaction, const UiRouteStackRevision expected,
                                                const UiRouteStackRevision next, const std::optional<UiRouteInstanceId> instance,
                                                const UiInteractionRevision interaction) noexcept
        : transaction_(std::move(transaction)), expected_(expected), next_(next), instance_(instance), admissionInteraction_(interaction) {}

    /** @copydoc UiScreenStack::AnimationTerminalProof::AnimationTerminalProof */
    UiScreenStack::AnimationTerminalProof::AnimationTerminalProof(const AnimationGate &gate,
                                                                  const UiInteractionRevision interaction) noexcept
        : stack_(gate.transaction_.storage_), operation_(gate.transaction_.operation_), revision_(gate.expected_),
          interaction_(interaction) {}

    /** @copydoc UiScreenStack::CanCloseAnimation */
    Result<void> UiScreenStack::CanCloseAnimation(const AnimationGate &gate) const {
        const auto &transaction = gate.transaction_;
        if (!storage_ || storage_->state != UiScreenStackState::Active || !storage_->busy || transaction.terminal_ ||
            transaction.storage_ != storage_ || storage_->activeOperation != transaction.operation_ || storage_->revision != gate.expected_)
            return Failure(UiErrors::RouteOperationStale);
        return Result<void>::Success();
    }

    /** @copydoc UiScreenStack::CloseAnimationValidated */
    UiRouteOperationResult UiScreenStack::CloseAnimationValidated(AnimationGate &gate, const UiRouteOperationRejection rejection) noexcept {
        const auto &transaction = gate.transaction_;
        const UiRouteOperationResult result{transaction.operation_,
                                            transaction.request_.kind,
                                            UiRouteOperationOutcome::Rejected,
                                            rejection,
                                            storage_->revision,
                                            {}};
        Finish(gate.transaction_);
        return result;
    }

    /** @copydoc UiScreenStack::PrepareAnimation */
    Result<UiScreenStack::AnimationGate> UiScreenStack::PrepareAnimation(UiRouteOperationRequest request,
                                                                         const UiInteractionRevision interaction) {
        if (!interaction.IsValid())
            return Failure<AnimationGate>(UiErrors::RouteOperationInvalid);
        auto prepared = Prepare(request);
        if (prepared.HasError())
            return Result<AnimationGate>::Failure(prepared.ErrorValue());
        auto transaction = std::move(prepared).Value();
        if (transaction.preparedRejection_ != UiRouteOperationRejection::None)
            return Result<AnimationGate>::Success(
                AnimationGate{std::move(transaction), storage_->revision, storage_->revision, {}, interaction});
        const bool changes = request.kind != UiRouteOperationKind::Clear || !storage_->routes.empty();
        const auto next = changes ? storage_->revision.Next() : Result<UiRouteStackRevision>::Success(storage_->revision);
        if (next.HasError())
            return Result<AnimationGate>::Failure(next.ErrorValue());
        std::optional<UiRouteInstanceId> instance;
        if (request.kind == UiRouteOperationKind::Push || request.kind == UiRouteOperationKind::Navigate ||
            request.kind == UiRouteOperationKind::Replace) {
            const auto issued = storage_->NextInstance();
            if (issued.HasError())
                return Result<AnimationGate>::Failure(issued.ErrorValue());
            instance = issued.Value();
        }
        return Result<AnimationGate>::Success(
            AnimationGate{std::move(transaction), storage_->revision, next.Value(), instance, interaction});
    }

    /** @copydoc UiScreenStack::CanPublishAnimation */
    Result<void> UiScreenStack::CanPublishAnimation(const AnimationGate &gate, const AnimationTerminalProof &proof) const {
        const auto &transaction = gate.transaction_;
        if (!storage_ || storage_->state != UiScreenStackState::Active || !storage_->busy || transaction.terminal_ ||
            transaction.storage_ != storage_ || storage_->activeOperation != transaction.operation_ ||
            transaction.preparedRejection_ != UiRouteOperationRejection::None)
            return Failure(UiErrors::RouteOperationLifecycleUnavailable);
        if (proof.stack_ != storage_ || proof.operation_ != transaction.operation_ || proof.revision_ != gate.expected_ ||
            storage_->revision != gate.expected_ || !proof.interaction_.IsValid() ||
            proof.interaction_.Compare(gate.admissionInteraction_) == UiRevisionRelation::Older)
            return Failure(UiErrors::RouteOperationStale);
        if (!storage_->CanRetireFor(transaction.request_.kind))
            return Failure(UiErrors::CapacityExceeded);
        return Result<void>::Success();
    }

    /** @copydoc UiScreenStack::PublishAnimationValidated */
    UiRouteOperationResult UiScreenStack::PublishAnimationValidated(AnimationGate &gate, const AnimationTerminalProof &) noexcept {
        auto &transaction = gate.transaction_;
        const auto route = ApplyIssuedMutation(*storage_, transaction, gate.instance_);
        storage_->revision = gate.next_;
        const UiRouteOperationResult result{transaction.operation_,          transaction.request_.kind, UiRouteOperationOutcome::Committed,
                                            UiRouteOperationRejection::None, storage_->revision,        route};
        Finish(transaction);
        return result;
    }

    /** @copydoc UiScreenStack::Prepare */
    Result<UiScreenStack::Transaction> UiScreenStack::Prepare(UiRouteOperationRequest request) {
        if (!storage_ || storage_->state != UiScreenStackState::Active)
            return Failure<Transaction>(UiErrors::RouteOperationLifecycleUnavailable);
        if (storage_->busy)
            return Failure<Transaction>(UiErrors::RouteOperationReentrant);
        if (const auto valid = request.Validate(); valid.HasError())
            return Result<Transaction>::Failure(valid.ErrorValue());

        const auto operation = storage_->NextOperation();
        if (operation.HasError())
            return Result<Transaction>::Failure(operation.ErrorValue());

        std::optional<UiRouteMetadata> definition;
        UiRouteOperationRejection rejection{UiRouteOperationRejection::None};
        if (request.route.has_value()) {
            definition = storage_->Find(*request.route);
            if (!definition.has_value())
                rejection = UiRouteOperationRejection::NotFound;
        }
        if (rejection == UiRouteOperationRejection::None && !request.guard.IsEmpty()) {
            if (request.guard.stack != storage_->stack || request.guard.revision != storage_->revision ||
                !IsSameTop(request.guard.top, storage_->routes))
                rejection = UiRouteOperationRejection::GuardMismatch;
        }
        if (rejection == UiRouteOperationRejection::None) {
            switch (request.kind) {
                case UiRouteOperationKind::Push:
                case UiRouteOperationKind::Navigate:
                    if (storage_->routes.size() == storage_->maximumRoutes)
                        rejection = UiRouteOperationRejection::Capacity;
                    break;
                case UiRouteOperationKind::Pop:
                case UiRouteOperationKind::Back:
                case UiRouteOperationKind::Replace:
                    if (storage_->routes.empty())
                        rejection = UiRouteOperationRejection::Empty;
                    break;
                case UiRouteOperationKind::Clear:
                case UiRouteOperationKind::Count:
                    break;
            }
        }
        if (rejection == UiRouteOperationRejection::None && !storage_->CanRetireFor(request.kind))
            rejection = UiRouteOperationRejection::Capacity;
        storage_->activeOperation = operation.Value();
        storage_->busy = true;
        return Result<Transaction>::Success(Transaction{storage_, request, operation.Value(), definition, rejection});
    }

    /** @copydoc UiScreenStack::Navigate */
    Result<UiRouteOperationResult> UiScreenStack::Navigate(UiRouteOperationRequest request) {
        auto prepared = Prepare(std::move(request));
        if (prepared.HasError())
            return Result<UiRouteOperationResult>::Failure(prepared.ErrorValue());
        return std::move(prepared).Value().Commit();
    }

    /** @copydoc UiScreenStack::Navigate */
    Result<UiRouteOperationResult> UiScreenStack::Navigate(const UiRouteId route, const UiRouteStackGuard &guard) {
        return Navigate(UiRouteOperationRequest::Navigate(route, guard));
    }

    /** @copydoc UiScreenStack::Push */
    Result<UiRouteOperationResult> UiScreenStack::Push(const UiRouteId route, const UiRouteStackGuard &guard) {
        return Navigate(UiRouteOperationRequest::Push(route, guard));
    }

    /** @copydoc UiScreenStack::Pop */
    Result<UiRouteOperationResult> UiScreenStack::Pop(const UiRouteStackGuard &guard) {
        return Navigate(UiRouteOperationRequest::Pop(guard));
    }

    /** @copydoc UiScreenStack::Replace */
    Result<UiRouteOperationResult> UiScreenStack::Replace(const UiRouteId route, const UiRouteStackGuard &guard) {
        return Navigate(UiRouteOperationRequest::Replace(route, guard));
    }

    /** @copydoc UiScreenStack::ReplaceTop */
    Result<UiRouteOperationResult> UiScreenStack::ReplaceTop(const UiRouteId route, const UiRouteStackGuard &guard) {
        return Replace(route, guard);
    }

    /** @copydoc UiScreenStack::Back */
    Result<UiRouteOperationResult> UiScreenStack::Back(const UiRouteStackGuard &guard) {
        return Navigate(UiRouteOperationRequest::Back(guard));
    }

    /** @copydoc UiScreenStack::Clear */
    Result<UiRouteOperationResult> UiScreenStack::Clear(const UiRouteStackGuard &guard) {
        return Navigate(UiRouteOperationRequest::Clear(guard));
    }

    /** @copydoc UiScreenStack::Reset */
    Result<UiRouteOperationResult> UiScreenStack::Reset(const UiRouteStackGuard &guard) {
        return Clear(guard);
    }

    /** @copydoc UiScreenStack::Commit */
    Result<UiRouteOperationResult> UiScreenStack::Commit(Transaction &transaction) {
        const auto storage = transaction.storage_;
        if (storage == nullptr || transaction.terminal_)
            return Failure<UiRouteOperationResult>(UiErrors::RouteOperationAlreadyCompleted);
        if (storage->state != UiScreenStackState::Active || !storage->busy || storage->activeOperation != transaction.operation_) {
            transaction.terminal_ = true;
            transaction.storage_ = nullptr;
            return Failure<UiRouteOperationResult>(UiErrors::RouteOperationLifecycleUnavailable);
        }
        if (transaction.preparedRejection_ != UiRouteOperationRejection::None) {
            const auto result = UiRouteOperationResult::Rejected(transaction.operation_, transaction.request_.kind, storage->revision,
                                                                 transaction.preparedRejection_);
            Finish(transaction);
            return result;
        }
        const bool changesStack = transaction.request_.kind != UiRouteOperationKind::Clear || !storage->routes.empty();
        UiRouteStackRevision nextRevision = storage->revision;
        if (changesStack) {
            const auto next = storage->revision.Next();
            if (next.HasError()) {
                Finish(transaction);
                return Failure<UiRouteOperationResult>(UiErrors::GenerationExhausted);
            }
            nextRevision = next.Value();
        }
        auto mutation = ApplyMutation(*storage, transaction);
        if (mutation.HasError()) {
            Finish(transaction);
            return Failure<UiRouteOperationResult>(UiErrors::GenerationExhausted);
        }
        storage->revision = nextRevision;
        const auto result =
            UiRouteOperationResult::Committed(transaction.operation_, transaction.request_.kind, storage->revision, mutation.Value());
        Finish(transaction);
        return result;
    }

    /** @copydoc UiScreenStack::Cancel */
    Result<UiRouteOperationResult> UiScreenStack::Cancel(Transaction &transaction) {
        const auto storage = transaction.storage_;
        if (storage == nullptr || transaction.terminal_)
            return Failure<UiRouteOperationResult>(UiErrors::RouteOperationAlreadyCompleted);
        if (storage->state == UiScreenStackState::Stopped || !storage->busy || storage->activeOperation != transaction.operation_) {
            transaction.terminal_ = true;
            transaction.storage_ = nullptr;
            return Failure<UiRouteOperationResult>(UiErrors::RouteOperationLifecycleUnavailable);
        }
        storage->busy = false;
        storage->activeOperation = {};
        transaction.terminal_ = true;
        transaction.storage_ = nullptr;
        return UiRouteOperationResult::Rejected(transaction.operation_, transaction.request_.kind, storage->revision,
                                                UiRouteOperationRejection::Cancelled);
    }

    /** @copydoc UiScreenStack::Abandon */
    void UiScreenStack::Abandon(Transaction &transaction) noexcept {
        if (transaction.storage_ != nullptr && transaction.storage_->busy &&
            transaction.storage_->activeOperation == transaction.operation_) {
            transaction.storage_->busy = false;
            transaction.storage_->activeOperation = {};
        }
        transaction.storage_ = nullptr;
        transaction.terminal_ = true;
    }

    /** @copydoc UiScreenStack::Stack */
    UiRouteStackId UiScreenStack::Stack() const noexcept {
        return storage_ ? storage_->stack : UiRouteStackId{};
    }

    /** @copydoc UiScreenStack::Revision */
    UiRouteStackRevision UiScreenStack::Revision() const noexcept {
        return storage_ ? storage_->revision : UiRouteStackRevision{};
    }

    /** @copydoc UiScreenStack::LastIssuedRouteIncarnation */
    std::uint32_t UiScreenStack::LastIssuedRouteIncarnation() const noexcept {
        return storage_ ? storage_->lastRouteIncarnation : 0;
    }

    /** @copydoc UiScreenStack::CanRetire */
    bool UiScreenStack::CanRetire() const noexcept {
        return storage_ && storage_->state == UiScreenStackState::Active && !storage_->busy;
    }

    /** @copydoc UiScreenStack::State */
    UiScreenStackState UiScreenStack::State() const noexcept {
        return storage_ ? storage_->state : UiScreenStackState::Stopped;
    }

    /** @copydoc UiScreenStack::Size */
    std::size_t UiScreenStack::Size() const noexcept {
        return storage_ ? storage_->routes.size() : 0;
    }

    /** @copydoc UiScreenStack::Empty */
    bool UiScreenStack::Empty() const noexcept {
        return Size() == 0;
    }

    /** @copydoc UiScreenStack::Top */
    std::optional<UiRouteInstance> UiScreenStack::Top() const {
        if (!storage_ || storage_->routes.empty())
            return std::nullopt;
        return storage_->routes.back();
    }

    /** @copydoc UiScreenStack::Definitions */
    std::span<const UiRouteMetadata> UiScreenStack::Definitions() const noexcept {
        return storage_ ? std::span<const UiRouteMetadata>{storage_->definitions} : std::span<const UiRouteMetadata>{};
    }

    /** @copydoc UiScreenStack::Routes */
    std::span<const UiRouteInstance> UiScreenStack::Routes() const noexcept {
        return storage_ ? std::span<const UiRouteInstance>{storage_->routes} : std::span<const UiRouteInstance>{};
    }

    /** @copydoc UiScreenStack::Guard */
    Result<UiRouteStackGuard> UiScreenStack::Guard() const {
        if (!storage_ || storage_->state == UiScreenStackState::Stopped)
            return Failure<UiRouteStackGuard>(UiErrors::RouteOperationLifecycleUnavailable);
        std::optional<UiRouteInstanceId> top;
        if (!storage_->routes.empty())
            top = storage_->routes.back().id;
        return UiRouteStackGuard::Create(storage_->stack, storage_->revision, top);
    }

    /** @copydoc UiScreenStack::AttachActions */
    Result<void> UiScreenStack::AttachActions(const UiRouteInstanceId route, UiActionRouter &&router) {
        if (!storage_ || storage_->state != UiScreenStackState::Active || storage_->busy)
            return Failure(UiErrors::RouteOperationLifecycleUnavailable);
        if (router.State() != UiActionRouterState::Active || router.Owner().instance.ownership != storage_->ownership)
            return Failure(UiErrors::ActionSourceStale);
        if (std::ranges::find(storage_->routes, route, &UiRouteInstance::id) == storage_->routes.end())
            return Failure(UiErrors::RouteOperationStale);
        if (Actions(route) != nullptr)
            return Failure(UiErrors::RouteOperationReentrant);
        storage_->actions.emplace_back(route, std::move(router));
        return Result<void>::Success();
    }

    /** @copydoc UiScreenStack::Actions */
    UiActionRouter *UiScreenStack::Actions(const UiRouteInstanceId route) noexcept {
        if (!storage_ || storage_->state != UiScreenStackState::Active)
            return nullptr;
        const auto found = std::ranges::find(storage_->actions, route, &Storage::RouteActions::route);
        return found == storage_->actions.end() ? nullptr : &found->router;
    }

    /** @copydoc UiScreenStack::BeginRetirement */
    Result<void> UiScreenStack::BeginRetirement() {
        if (!storage_ || storage_->state != UiScreenStackState::Active)
            return Failure(UiErrors::RouteOperationLifecycleUnavailable);
        if (storage_->busy)
            return Failure(UiErrors::RouteOperationReentrant);
        storage_->state = UiScreenStackState::Retiring;
        for (auto &actions : storage_->actions)
            if (actions.router.State() == UiActionRouterState::Active)
                (void)actions.router.BeginRetirement(UiActionCancellationReason::OwnerRetired);
        return Result<void>::Success();
    }

    /** @copydoc UiScreenStack::Shutdown */
    void UiScreenStack::Shutdown() noexcept {
        if (!storage_)
            return;
        storage_->state = UiScreenStackState::Stopped;
        storage_->busy = false;
        storage_->activeOperation = {};
        for (auto &actions : storage_->actions)
            actions.router.Shutdown();
        // Actual router ownership remains pinned until an explicit quiescent drain or final stack destruction.
        storage_->routes.clear();
        storage_->definitions.clear();
    }

    /** @copydoc UiScreenStack::DrainRetiredActions */
    Result<std::size_t> UiScreenStack::DrainRetiredActions() {
        if (!storage_)
            return Failure<std::size_t>(UiErrors::RouteOperationLifecycleUnavailable);
        if (storage_->busy)
            return Failure<std::size_t>(UiErrors::RouteOperationReentrant);
        std::size_t reclaimed = storage_->retiredActions.size();
        for (auto &actions : storage_->actions)
            reclaimed += actions.router.DrainInteractionReplacement();
        for (auto &actions : storage_->retiredActions)
            actions.router.Shutdown();
        storage_->retiredActions.clear();
        if (storage_->state == UiScreenStackState::Stopped) {
            reclaimed += storage_->actions.size();
            storage_->actions.clear();
        }
        return Result<std::size_t>::Success(reclaimed);
    }

    /** @copydoc UiScreenStack::ReserveActionInteractionReplacements */
    Result<void> UiScreenStack::ReserveActionInteractionReplacements() {
        if (!storage_ || storage_->state != UiScreenStackState::Active || storage_->busy)
            return Failure(UiErrors::RouteOperationLifecycleUnavailable);
        for (auto &actions : storage_->actions) {
            if (auto reserved = actions.router.ReserveInteractionReplacement(); reserved.HasError())
                return reserved;
        }
        return Result<void>::Success();
    }

    /** @copydoc UiScreenStack::PrepareActionInteractionReplacements */
    Result<void> UiScreenStack::PrepareActionInteractionReplacements(const UiActionOwnerContext &owner) {
        if (!storage_ || storage_->state != UiScreenStackState::Active)
            return Failure(UiErrors::RouteOperationLifecycleUnavailable);
        for (auto &actions : storage_->actions) {
            if (auto prepared = actions.router.PrepareInteractionReplacement(owner); prepared.HasError()) {
                AbandonActionInteractionReplacements();
                return prepared;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc UiScreenStack::CanPublishActionInteractionReplacements */
    Result<void> UiScreenStack::CanPublishActionInteractionReplacements(const UiActionOwnerContext &owner) const {
        if (!storage_ || storage_->state != UiScreenStackState::Active)
            return Failure(UiErrors::RouteOperationLifecycleUnavailable);
        for (const auto &actions : storage_->actions) {
            if (auto valid = actions.router.CanPublishInteractionReplacement(owner); valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc UiScreenStack::PublishActionInteractionReplacements */
    void UiScreenStack::PublishActionInteractionReplacements() noexcept {
        for (auto &actions : storage_->actions)
            actions.router.PublishInteractionReplacement();
    }

    /** @copydoc UiScreenStack::AbandonActionInteractionReplacements */
    void UiScreenStack::AbandonActionInteractionReplacements() noexcept {
        if (storage_)
            for (auto &actions : storage_->actions)
                actions.router.AbandonInteractionReplacement();
    }

}  // namespace Horo::Runtime::Ui
