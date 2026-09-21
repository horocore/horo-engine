#include "Horo/Runtime/Ui/UiScreenStack.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        template <typename Enum> [[nodiscard]] bool IsKnown(const Enum value, const Enum count) noexcept {
            return static_cast<std::underlying_type_t<Enum>>(value) < static_cast<std::underlying_type_t<Enum>>(count);
        }

        [[nodiscard]] bool IsValidRouteMetadata(const UiRouteMetadata &route) noexcept {
            return route.id.IsValid() && IsKnown(route.band, UiPresentationBand::Count);
        }

        [[nodiscard]] bool HasGuardField(const UiRouteStackGuard &guard) noexcept {
            return guard.stack.IsValid() || guard.revision.IsValid() || guard.top.has_value();
        }

        [[nodiscard]] bool IsSameTop(const std::optional<UiRouteInstanceId> &expected,
                                     const std::vector<UiRouteInstance> &routes) noexcept {
            if (!expected.has_value())
                return routes.empty();
            return !routes.empty() && routes.back().id == *expected;
        }
    }  // namespace

    /** @copydoc UiRouteStackGuard::Create */
    Result<UiRouteStackGuard> UiRouteStackGuard::Create(const UiRouteStackId stack, const UiRouteStackRevision revision,
                                                        const std::optional<UiRouteInstanceId> top) {
        UiRouteStackGuard guard{stack, revision, top};
        if (!guard.IsValid())
            return Failure<UiRouteStackGuard>(UiErrors::RouteOperationInvalid);
        return Result<UiRouteStackGuard>::Success(guard);
    }

    /** @copydoc UiRouteStackGuard::IsEmpty */
    bool UiRouteStackGuard::IsEmpty() const noexcept {
        return !stack.IsValid() && !revision.IsValid() && !top.has_value();
    }

    /** @copydoc UiRouteStackGuard::IsValid */
    bool UiRouteStackGuard::IsValid() const noexcept {
        return stack.IsValid() && revision.IsValid() && (!top.has_value() || (top->IsValid() && top->ownership == stack.ownership));
    }

    /** @copydoc UiRouteOperationId::IsValid */
    bool UiRouteOperationId::IsValid() const noexcept {
        return ownership.IsValid() && sequence.IsValid();
    }

    /** @copydoc UiRouteOperationRequest::Push */
    UiRouteOperationRequest UiRouteOperationRequest::Push(const UiRouteId route, const UiRouteStackGuard guard) {
        return {UiRouteOperationKind::Push, route, guard};
    }

    /** @copydoc UiRouteOperationRequest::Pop */
    UiRouteOperationRequest UiRouteOperationRequest::Pop(const UiRouteStackGuard guard) {
        return {UiRouteOperationKind::Pop, std::nullopt, guard};
    }

    /** @copydoc UiRouteOperationRequest::Replace */
    UiRouteOperationRequest UiRouteOperationRequest::Replace(const UiRouteId route, const UiRouteStackGuard guard) {
        return {UiRouteOperationKind::Replace, route, guard};
    }

    /** @copydoc UiRouteOperationRequest::ReplaceTop */
    UiRouteOperationRequest UiRouteOperationRequest::ReplaceTop(const UiRouteId route, const UiRouteStackGuard guard) {
        return Replace(route, guard);
    }

    /** @copydoc UiRouteOperationRequest::Back */
    UiRouteOperationRequest UiRouteOperationRequest::Back(const UiRouteStackGuard guard) {
        return {UiRouteOperationKind::Back, std::nullopt, guard};
    }

    /** @copydoc UiRouteOperationRequest::Clear */
    UiRouteOperationRequest UiRouteOperationRequest::Clear(const UiRouteStackGuard guard) {
        return {UiRouteOperationKind::Clear, std::nullopt, guard};
    }

    /** @copydoc UiRouteOperationRequest::Reset */
    UiRouteOperationRequest UiRouteOperationRequest::Reset(const UiRouteStackGuard guard) {
        return Clear(guard);
    }

    /** @copydoc UiRouteOperationRequest::Navigate */
    UiRouteOperationRequest UiRouteOperationRequest::Navigate(const UiRouteId route, const UiRouteStackGuard guard) {
        return {UiRouteOperationKind::Navigate, route, guard};
    }

    /** @copydoc UiRouteOperationRequest::Validate */
    Result<void> UiRouteOperationRequest::Validate() const {
        if (!IsKnown(kind, UiRouteOperationKind::Count))
            return Failure(UiErrors::RouteOperationInvalid);
        const bool needsRoute =
            kind == UiRouteOperationKind::Push || kind == UiRouteOperationKind::Replace || kind == UiRouteOperationKind::Navigate;
        if (needsRoute != route.has_value() || (route.has_value() && !route->IsValid()))
            return Failure(UiErrors::RouteOperationInvalid);
        if (kind == UiRouteOperationKind::Navigate && guard.IsEmpty())
            return Failure(UiErrors::RouteOperationInvalid);
        if (HasGuardField(guard) && !guard.IsValid())
            return Failure(UiErrors::RouteOperationInvalid);
        return Result<void>::Success();
    }

    /** @copydoc UiRouteOperationResult::Committed */
    Result<UiRouteOperationResult> UiRouteOperationResult::Committed(const UiRouteOperationId operation, const UiRouteOperationKind kind,
                                                                     const UiRouteStackRevision revision,
                                                                     const std::optional<UiRouteInstanceId> route) {
        UiRouteOperationResult result{operation, kind, UiRouteOperationOutcome::Committed, UiRouteOperationRejection::None,
                                      revision,  route};
        if (const auto valid = result.Validate(); valid.HasError())
            return Failure<UiRouteOperationResult>(UiErrors::RouteOperationInvalid);
        return Result<UiRouteOperationResult>::Success(result);
    }

    /** @copydoc UiRouteOperationResult::Rejected */
    Result<UiRouteOperationResult> UiRouteOperationResult::Rejected(const UiRouteOperationId operation, const UiRouteOperationKind kind,
                                                                    const UiRouteStackRevision revision,
                                                                    const UiRouteOperationRejection rejection) {
        if (!IsKnown(rejection, UiRouteOperationRejection::Count) || rejection == UiRouteOperationRejection::None)
            return Failure<UiRouteOperationResult>(UiErrors::RouteOperationInvalid);
        UiRouteOperationResult result{operation, kind, UiRouteOperationOutcome::Rejected, rejection, revision, std::nullopt};
        if (const auto valid = result.Validate(); valid.HasError())
            return Failure<UiRouteOperationResult>(UiErrors::RouteOperationInvalid);
        return Result<UiRouteOperationResult>::Success(result);
    }

    /** @copydoc UiRouteOperationResult::Validate */
    Result<void> UiRouteOperationResult::Validate() const {
        if (!operation.IsValid() || !IsKnown(kind, UiRouteOperationKind::Count) || !revision.IsValid() ||
            !IsKnown(outcome, UiRouteOperationOutcome::Count))
            return Failure(UiErrors::RouteOperationInvalid);
        if (route.has_value() && !route->IsValid())
            return Failure(UiErrors::RouteOperationInvalid);
        if (outcome == UiRouteOperationOutcome::Committed)
            return rejection == UiRouteOperationRejection::None ? Result<void>::Success() : Failure(UiErrors::RouteOperationInvalid);
        if (outcome != UiRouteOperationOutcome::Rejected || !IsKnown(rejection, UiRouteOperationRejection::Count) ||
            rejection == UiRouteOperationRejection::None || route.has_value())
            return Failure(UiErrors::RouteOperationInvalid);
        return Result<void>::Success();
    }

    /** @copydoc UiRouteOperationResult::IsTerminal */
    bool UiRouteOperationResult::IsTerminal() const noexcept {
        return outcome == UiRouteOperationOutcome::Committed || outcome == UiRouteOperationOutcome::Rejected;
    }

    /** @copydoc UiRouteOperationResult::IsCommitted */
    bool UiRouteOperationResult::IsCommitted() const noexcept {
        return outcome == UiRouteOperationOutcome::Committed;
    }

    /** @copydoc UiScreenStackDescriptor::IsValid */
    bool UiScreenStackDescriptor::IsValid() const noexcept {
        if (!ownership.IsValid() || !stack.IsValid() || stack.ownership != ownership || maximumRoutes == 0 ||
            maximumRoutes > MaximumUiScreenStackRoutes || definitions.size() > MaximumUiScreenStackRoutes)
            return false;
        for (std::size_t index = 0; index < definitions.size(); ++index) {
            if (!IsValidRouteMetadata(definitions[index]))
                return false;
            for (std::size_t prior = 0; prior < index; ++prior)
                if (definitions[prior].id == definitions[index].id)
                    return false;
        }
        return true;
    }

    struct UiScreenStack::Storage final {
        Storage(const UiScreenStackDescriptor &descriptor)
            : ownership(descriptor.ownership), stack(descriptor.stack),
              definitions(descriptor.definitions.begin(), descriptor.definitions.end()) {
            routes.reserve(descriptor.maximumRoutes);
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
            if (nextInstanceSlot == 0)
                return Failure<UiRouteInstanceId>(UiErrors::GenerationExhausted);
            const UiRouteInstanceId instance{ownership, nextInstanceSlot, 1};
            if (nextInstanceSlot == std::numeric_limits<std::uint32_t>::max())
                nextInstanceSlot = 0;
            else
                ++nextInstanceSlot;
            return Result<UiRouteInstanceId>::Success(instance);
        }

        UiOwnershipGeneration ownership;
        UiRouteStackId stack;
        std::vector<UiRouteMetadata> definitions;
        std::vector<UiRouteInstance> routes;
        UiRouteStackRevision revision{UiRouteStackRevision::Create(1).Value()};
        std::uint32_t nextInstanceSlot{1};
        std::uint64_t nextOperationSequence{1};
        UiRouteOperationId activeOperation;
        UiScreenStackState state{UiScreenStackState::Active};
        bool busy{};
    };

    /** @copydoc UiScreenStack::Transaction::Transaction */
    UiScreenStack::Transaction::Transaction(Storage *storage, const UiRouteOperationRequest request, const UiRouteOperationId operation,
                                            const std::optional<UiRouteMetadata> definition,
                                            const UiRouteOperationRejection preparedRejection) noexcept
        : storage_(storage), request_(request), operation_(operation), definition_(definition), preparedRejection_(preparedRejection) {}

    /** @copydoc UiScreenStack::Transaction::~Transaction */
    UiScreenStack::Transaction::~Transaction() {
        Abandon();
    }

    /** @copydoc UiScreenStack::Transaction::Transaction */
    UiScreenStack::Transaction::Transaction(Transaction &&other) noexcept
        : storage_(other.storage_), request_(other.request_), operation_(other.operation_), definition_(other.definition_),
          preparedRejection_(other.preparedRejection_), terminal_(other.terminal_) {
        other.storage_ = nullptr;
        other.terminal_ = true;
    }

    /** @copydoc UiScreenStack::Transaction::operator= */
    UiScreenStack::Transaction &UiScreenStack::Transaction::operator=(Transaction &&other) noexcept {
        if (this == &other)
            return *this;
        Abandon();
        storage_ = other.storage_;
        request_ = other.request_;
        operation_ = other.operation_;
        definition_ = other.definition_;
        preparedRejection_ = other.preparedRejection_;
        terminal_ = other.terminal_;
        other.storage_ = nullptr;
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
            return Result<UiScreenStack>::Success(UiScreenStack{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiScreenStack>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiScreenStack::UiScreenStack */
    UiScreenStack::UiScreenStack(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiScreenStack::~UiScreenStack */
    UiScreenStack::~UiScreenStack() {
        Shutdown();
    }

    /** @copydoc UiScreenStack::UiScreenStack */
    UiScreenStack::UiScreenStack(UiScreenStack &&) noexcept = default;

    /** @copydoc UiScreenStack::operator= */
    UiScreenStack &UiScreenStack::operator=(UiScreenStack &&) noexcept = default;

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
                    if (storage_->routes.size() == storage_->routes.capacity())
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
        storage_->activeOperation = operation.Value();
        storage_->busy = true;
        return Result<Transaction>::Success(Transaction{storage_.get(), request, operation.Value(), definition, rejection});
    }

    /** @copydoc UiScreenStack::Navigate */
    Result<UiRouteOperationResult> UiScreenStack::Navigate(UiRouteOperationRequest request) {
        auto prepared = Prepare(std::move(request));
        if (prepared.HasError())
            return Result<UiRouteOperationResult>::Failure(prepared.ErrorValue());
        return std::move(prepared).Value().Commit();
    }

    /** @copydoc UiScreenStack::Navigate */
    Result<UiRouteOperationResult> UiScreenStack::Navigate(const UiRouteId route, const UiRouteStackGuard guard) {
        return Navigate(UiRouteOperationRequest::Navigate(route, guard));
    }

    /** @copydoc UiScreenStack::Push */
    Result<UiRouteOperationResult> UiScreenStack::Push(const UiRouteId route, const UiRouteStackGuard guard) {
        return Navigate(UiRouteOperationRequest::Push(route, guard));
    }

    /** @copydoc UiScreenStack::Pop */
    Result<UiRouteOperationResult> UiScreenStack::Pop(const UiRouteStackGuard guard) {
        return Navigate(UiRouteOperationRequest::Pop(guard));
    }

    /** @copydoc UiScreenStack::Replace */
    Result<UiRouteOperationResult> UiScreenStack::Replace(const UiRouteId route, const UiRouteStackGuard guard) {
        return Navigate(UiRouteOperationRequest::Replace(route, guard));
    }

    /** @copydoc UiScreenStack::ReplaceTop */
    Result<UiRouteOperationResult> UiScreenStack::ReplaceTop(const UiRouteId route, const UiRouteStackGuard guard) {
        return Replace(route, guard);
    }

    /** @copydoc UiScreenStack::Back */
    Result<UiRouteOperationResult> UiScreenStack::Back(const UiRouteStackGuard guard) {
        return Navigate(UiRouteOperationRequest::Back(guard));
    }

    /** @copydoc UiScreenStack::Clear */
    Result<UiRouteOperationResult> UiScreenStack::Clear(const UiRouteStackGuard guard) {
        return Navigate(UiRouteOperationRequest::Clear(guard));
    }

    /** @copydoc UiScreenStack::Reset */
    Result<UiRouteOperationResult> UiScreenStack::Reset(const UiRouteStackGuard guard) {
        return Clear(guard);
    }

    /** @copydoc UiScreenStack::Commit */
    Result<UiRouteOperationResult> UiScreenStack::Commit(Transaction &transaction) {
        Storage *storage = transaction.storage_;
        if (storage == nullptr || transaction.terminal_)
            return Failure<UiRouteOperationResult>(UiErrors::RouteOperationAlreadyCompleted);
        if (storage->state != UiScreenStackState::Active || !storage->busy || storage->activeOperation != transaction.operation_) {
            transaction.terminal_ = true;
            transaction.storage_ = nullptr;
            return Failure<UiRouteOperationResult>(UiErrors::RouteOperationLifecycleUnavailable);
        }

        const auto finish = [&transaction, storage]() noexcept {
            storage->busy = false;
            storage->activeOperation = {};
            transaction.terminal_ = true;
            transaction.storage_ = nullptr;
        };
        if (transaction.preparedRejection_ != UiRouteOperationRejection::None) {
            const auto result = UiRouteOperationResult::Rejected(transaction.operation_, transaction.request_.kind, storage->revision,
                                                                 transaction.preparedRejection_);
            finish();
            return result;
        }

        const bool changesStack = transaction.request_.kind != UiRouteOperationKind::Clear || !storage->routes.empty();
        UiRouteStackRevision nextRevision = storage->revision;
        if (changesStack) {
            const auto next = storage->revision.Next();
            if (next.HasError()) {
                finish();
                return Failure<UiRouteOperationResult>(UiErrors::GenerationExhausted);
            }
            nextRevision = next.Value();
        }

        std::optional<UiRouteInstanceId> resultRoute;
        switch (transaction.request_.kind) {
            case UiRouteOperationKind::Push:
            case UiRouteOperationKind::Navigate: {
                const auto instance = storage->NextInstance();
                if (instance.HasError()) {
                    finish();
                    return Failure<UiRouteOperationResult>(UiErrors::GenerationExhausted);
                }
                for (auto &route : storage->routes)
                    route.visibility = UiRouteVisibilityState::Covered;
                storage->routes.push_back({instance.Value(), *transaction.definition_, UiRouteVisibilityState::Visible});
                resultRoute = instance.Value();
                break;
            }
            case UiRouteOperationKind::Pop:
            case UiRouteOperationKind::Back:
                storage->routes.pop_back();
                if (!storage->routes.empty()) {
                    storage->routes.back().visibility = UiRouteVisibilityState::Visible;
                    resultRoute = storage->routes.back().id;
                }
                break;
            case UiRouteOperationKind::Replace: {
                const auto instance = storage->NextInstance();
                if (instance.HasError()) {
                    finish();
                    return Failure<UiRouteOperationResult>(UiErrors::GenerationExhausted);
                }
                storage->routes.back() = {instance.Value(), *transaction.definition_, UiRouteVisibilityState::Visible};
                resultRoute = instance.Value();
                break;
            }
            case UiRouteOperationKind::Clear:
                storage->routes.clear();
                break;
            case UiRouteOperationKind::Count:
                finish();
                return Failure<UiRouteOperationResult>(UiErrors::RouteOperationInvalid);
        }
        storage->revision = nextRevision;
        const auto result =
            UiRouteOperationResult::Committed(transaction.operation_, transaction.request_.kind, storage->revision, resultRoute);
        finish();
        return result;
    }

    /** @copydoc UiScreenStack::Cancel */
    Result<UiRouteOperationResult> UiScreenStack::Cancel(Transaction &transaction) {
        Storage *storage = transaction.storage_;
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

    /** @copydoc UiScreenStack::BeginRetirement */
    Result<void> UiScreenStack::BeginRetirement() {
        if (!storage_ || storage_->state != UiScreenStackState::Active)
            return Failure(UiErrors::RouteOperationLifecycleUnavailable);
        if (storage_->busy)
            return Failure(UiErrors::RouteOperationReentrant);
        storage_->state = UiScreenStackState::Retiring;
        return Result<void>::Success();
    }

    /** @copydoc UiScreenStack::Shutdown */
    void UiScreenStack::Shutdown() noexcept {
        if (!storage_)
            return;
        storage_->state = UiScreenStackState::Stopped;
        storage_->busy = false;
        storage_->activeOperation = {};
        storage_->routes.clear();
        storage_->definitions.clear();
    }
}  // namespace Horo::Runtime::Ui
