#include "Horo/Gameplay/GameplayNavigationContext.h"

#include <algorithm>
#include <limits>
#include <new>
#include <thread>

namespace Horo::Gameplay {
    namespace NavigationCapabilityErrors {
        const ErrorCodeDescriptor PermissionDenied{.domain = ErrorDomainId{"horo.gameplay"},
                                                   .code = ErrorCode{"gameplay.navigation.permission_denied"},
                                                   .summary = "Navigation operation is not granted."};
        const ErrorCodeDescriptor ScopeRevoked{.domain = ErrorDomainId{"horo.gameplay"},
                                               .code = ErrorCode{"gameplay.navigation.scope_revoked"},
                                               .summary = "Navigation scope has retired."};
        const ErrorCodeDescriptor WrongThread{.domain = ErrorDomainId{"horo.gameplay"},
                                              .code = ErrorCode{"gameplay.navigation.wrong_thread"},
                                              .summary = "Navigation requires its owner thread."};
    }  // namespace NavigationCapabilityErrors

    struct GameplayNavigationRecord final {
        CancellationSource cancellation;
        std::optional<Navigation::NavRequestHandle> query;
        std::optional<Navigation::NavigationMovementId> movement;
        std::shared_ptr<GameplayNavigationResult> storage;
        std::shared_ptr<const GameplayNavigationResult> completed;
        std::optional<Error> error;
    };

    namespace {
        /** @brief Allocate result ownership in the composing host image, independent of native SDK call sites. */
        std::shared_ptr<GameplayNavigationResult> MakeHostResult() {
            return std::make_shared<GameplayNavigationResult>();
        }
    }  // namespace

    struct GameplayNavigationState final {
        std::shared_ptr<void> codeImage; /**< Declared first, destroyed last after SDK-origin record and token deleters. */
        std::shared_ptr<GameplayNavigationResult> (*allocateResult)(){}; /**< Captured by host Create, never rebound by SDK submission. */
        GameplayNavigationBinding binding;
        Navigation::NavigationWorldLifecycle *world{};
        Navigation::NavigationCoordinator *coordinator{};
        Navigation::NavigationMovementController *movement{};
        Navigation::NavigationPathSubmission policy;
        std::optional<Navigation::NavigationMovementObservation> current;
        std::vector<std::shared_ptr<GameplayNavigationRecord>> records;
        std::thread::id thread{std::this_thread::get_id()};
        std::size_t maximum{};
        bool revoked{};

        /** @brief Reject retired scopes before touching any borrowed host object. */
        Result<void> Check() const {
            if (thread != std::this_thread::get_id())
                return Result<void>::Failure(MakeError(NavigationCapabilityErrors::WrongThread));
            if (revoked)
                return Result<void>::Failure(MakeError(NavigationCapabilityErrors::ScopeRevoked));
            return Result<void>::Success();
        }

        /** @brief Validate operation admission in the same order for native and script callers. */
        Result<void> ValidateRequest(const GameplayNavigationRequest &request) const {
            if (auto check = Check(); check.HasError())
                return check;
            auto failure = [](const ErrorCodeDescriptor &error) {
                return Result<void>::Failure(MakeError(error));
            };
            if (request.operation >= GameplayNavigationOperation::Count || !Math::IsFinite(request.start) ||
                !Math::IsFinite(request.destination))
                return failure(Navigation::NavigationErrors::CapabilityDescriptorInvalid);
            const bool query = request.operation == GameplayNavigationOperation::Path;
            if (!(query ? binding.queryGranted : binding.movementGranted))
                return failure(NavigationCapabilityErrors::PermissionDenied);
            if (!coordinator || (!query && !movement))
                return failure(Navigation::NavigationErrors::CapabilityUnavailable);
            if (!current)
                return failure(Navigation::NavigationErrors::StaleSnapshot);
            if (records.size() >= maximum)
                return failure(Navigation::NavigationErrors::AdmissionRejected);
            if (request.cancellation.IsCancellationRequested())
                return failure(Navigation::NavigationErrors::QueryCancelled);
            return Result<void>::Success();
        }

        /** @brief Forward host query policy and exact copied caller context to the canonical coordinator. */
        Result<Navigation::NavRequestHandle> SubmitPath(GameplayNavigationRequest &request, CancellationToken cancellation) {
            auto submission = policy;
            if (submission.deadlineTick <= submission.targetTick ||
                current->path.tick > std::numeric_limits<std::uint64_t>::max() - submission.deadlineTick)
                return Result<Navigation::NavRequestHandle>::Failure(MakeError(Navigation::NavigationErrors::CapabilityDescriptorInvalid));
            submission.targetTick += current->path.tick;
            submission.deadlineTick += current->path.tick;
            submission.source = current->path.provenance;
            submission.request.world = current->binding.world;
            submission.request.topology = current->path.provenance.topology;
            submission.request.start = request.start;
            submission.request.destination = request.destination;
            submission.cancellation = std::move(cancellation);
            submission.operation = std::move(request.context);
            return coordinator->Submit(*world, std::move(submission), current->path.tick);
        }

        /** @brief Forward the same owner observation and request lifetime into canonical movement. */
        Result<Navigation::NavigationMovementId> SubmitMovement(GameplayNavigationRequest &request, CancellationToken cancellation) {
            Navigation::NavigationMovementRequest move{.mode = request.operation == GameplayNavigationOperation::MoveTo
                                                                   ? Navigation::NavigationMovementMode::MoveTo
                                                                   : Navigation::NavigationMovementMode::Follow,
                                                       .destination = request.destination,
                                                       .entity = request.target,
                                                       .query = policy.request.requirement,
                                                       .capabilityRevision = policy.capabilityRevision,
                                                       .cancellation = std::move(cancellation),
                                                       .operation = std::move(request.context)};
            auto accepted = movement->Start(std::move(move), *current);
            if (accepted.HasError())
                return Result<Navigation::NavigationMovementId>::Failure(accepted.ErrorValue());
            return Result<Navigation::NavigationMovementId>::Success(accepted.Value().accepted);
        }

        /** @brief Transfer canonical owner-published records, preserving terminal errors and replacement identities. */
        void Poll(GameplayNavigationRecord &record) {
            if (record.completed || record.error)
                return;
            if (record.query) {
                if (record.cancellation.Token().IsCancellationRequested())
                    static_cast<void>(coordinator->Cancel(*record.query));
                if (auto result = coordinator->Take(*record.query)) {
                    if (result->result.HasError())
                        record.error = std::move(result->result).ErrorValue();
                    else {
                        record.storage->path = std::move(result->result).Value();
                        record.completed = record.storage;
                    }
                }
            } else if (record.movement) {
                using enum Navigation::NavigationMovementState;
                if (record.cancellation.Token().IsCancellationRequested())
                    static_cast<void>(movement->Cancel(*record.movement));
                const auto &currentStatus = movement->Status();
                if (currentStatus.id == *record.movement && currentStatus.state < Arrived)
                    return;
                auto status = currentStatus;
                if (status.id != *record.movement)
                    status = {*record.movement, Superseded, {}};
                if (status.state >= Arrived) {
                    record.storage->movement = std::move(status);
                    record.completed = record.storage;
                }
            }
        }
    };

    GameplayNavigationHandle::GameplayNavigationHandle(std::shared_ptr<GameplayNavigationState> owner,
                                                       std::shared_ptr<GameplayNavigationRecord> record)
        : owner_(std::move(owner)), record_(std::move(record)) {}

    /** @copydoc GameplayNavigationHandle::Poll */
    Result<std::shared_ptr<const GameplayNavigationResult>> GameplayNavigationHandle::Poll() const {
        if (!owner_ || !record_)
            return Result<std::shared_ptr<const GameplayNavigationResult>>::Failure(MakeError(Navigation::NavigationErrors::InvalidHandle));
        if (auto check = owner_->Check(); check.HasError())
            return Result<std::shared_ptr<const GameplayNavigationResult>>::Failure(check.ErrorValue());
        owner_->Poll(*record_);
        if (record_->error)
            return Result<std::shared_ptr<const GameplayNavigationResult>>::Failure(*record_->error);
        return Result<std::shared_ptr<const GameplayNavigationResult>>::Success(record_->completed);
    }

    /** @copydoc GameplayNavigationHandle::Cancel */
    Result<bool> GameplayNavigationHandle::Cancel() const {
        if (!owner_ || !record_)
            return Result<bool>::Failure(MakeError(Navigation::NavigationErrors::InvalidHandle));
        if (auto check = owner_->Check(); check.HasError())
            return Result<bool>::Failure(check.ErrorValue());
        if (record_->completed || record_->error)
            return Result<bool>::Success(false);
        record_->cancellation.RequestCancellation();
        owner_->Poll(*record_);
        return Result<bool>::Success(true);
    }

    /** @copydoc GameplayNavigationContext::GameplayNavigationContext */
    GameplayNavigationContext::GameplayNavigationContext(CreationKey, std::shared_ptr<GameplayNavigationState> state)
        : state_(std::move(state)) {}

    /** @copydoc GameplayNavigationContext::Create */
    Result<std::shared_ptr<GameplayNavigationContext>> GameplayNavigationContext::Create(
        GameplayNavigationBinding binding, Navigation::NavigationWorldLifecycle *world, Navigation::NavigationCoordinator *coordinator,
        Navigation::NavigationMovementController *movement, Navigation::NavigationPathSubmission policy, const std::size_t maximumPending) {
        try {
            if (binding.moduleId.empty() || binding.moduleId.size() > 256 || binding.moduleId.find('\0') != std::string::npos ||
                maximumPending == 0 || maximumPending > 64 || (world == nullptr) != (coordinator == nullptr) ||
                (movement && (movement->Binding() != binding.scene || !coordinator)))
                return Result<std::shared_ptr<GameplayNavigationContext>>::Failure(
                    MakeError(Navigation::NavigationErrors::CapabilityDescriptorInvalid));
            if (world) {
                auto lease = world->Acquire(binding.scene.world);
                if (lease.HasError())
                    return Result<std::shared_ptr<GameplayNavigationContext>>::Failure(lease.ErrorValue());
                if (lease.Value().Descriptor().scene != binding.scene.scene ||
                    lease.Value().Descriptor().sceneGeneration != binding.scene.sceneGeneration)
                    return Result<std::shared_ptr<GameplayNavigationContext>>::Failure(
                        MakeError(Navigation::NavigationErrors::InvalidWorld));
            }
            auto state = std::make_shared<GameplayNavigationState>();
            state->allocateResult = &MakeHostResult;
            state->binding = std::move(binding);
            state->world = world;
            state->coordinator = coordinator;
            state->movement = movement;
            state->policy = std::move(policy);
            state->maximum = maximumPending;
            state->records.reserve(maximumPending);
            return Result<std::shared_ptr<GameplayNavigationContext>>::Success(
                std::make_shared<GameplayNavigationContext>(CreationKey{}, std::move(state)));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<GameplayNavigationContext>>::Failure(MakeError(Navigation::NavigationErrors::CapacityExceeded));
        }
    }

    GameplayNavigationContext::~GameplayNavigationContext() {
        Revoke();
    }

    /** @copydoc GameplayNavigationContext::DrainRetired */
    Result<bool> GameplayNavigationContext::DrainRetired() const {
        if (state_->thread != std::this_thread::get_id())
            return Result<bool>::Failure(MakeError(NavigationCapabilityErrors::WrongThread));
        if (!state_->revoked)
            return Result<bool>::Success(false);
        for (const auto &record : state_->records)
            state_->Poll(*record);
        std::erase_if(state_->records, [](const auto &record) {
            return record->completed || record->error;
        });
        if (!state_->records.empty() || (state_->coordinator && !state_->coordinator->IsDrained()))
            return Result<bool>::Success(false);
        state_->world = nullptr;
        state_->coordinator = nullptr;
        state_->movement = nullptr;
        return Result<bool>::Success(true);
    }

    /** @copydoc GameplayNavigationContext::RetainCodeImage */
    Result<void> GameplayNavigationContext::RetainCodeImage(std::shared_ptr<void> image) const {
        if (auto admission = state_->Check(); admission.HasError())
            return admission;
        if (state_->codeImage && state_->codeImage != image)
            return Result<void>::Failure(MakeError(NavigationCapabilityErrors::PermissionDenied));
        state_->codeImage = std::move(image);
        return Result<void>::Success();
    }

    /** @copydoc GameplayNavigationContext::Binding */
    const GameplayNavigationBinding &GameplayNavigationContext::Binding() const noexcept {
        return state_->binding;
    }

    /** @copydoc GameplayNavigationContext::Refresh */
    Result<void> GameplayNavigationContext::Refresh(const Navigation::NavigationMovementObservation &current) const {
        if (auto check = state_->Check(); check.HasError())
            return check;
        if (current.binding != state_->binding.scene || !Navigation::ValidateNavigationOutcomeProvenance(current.path.provenance) ||
            current.path.provenance.world != current.binding.world || (state_->current && current.path.tick < state_->current->path.tick))
            return Result<void>::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));
        state_->current = current;
        for (const auto &record : state_->records)
            state_->Poll(*record);
        std::erase_if(state_->records, [](const auto &record) {
            return record->completed || record->error;
        });
        return Result<void>::Success();
    }

    /** @copydoc GameplayNavigationContext::Submit */
    Result<GameplayNavigationHandle> GameplayNavigationContext::Submit(GameplayNavigationRequest request) const {
        try {
            if (auto admission = state_->ValidateRequest(request); admission.HasError())
                return Result<GameplayNavigationHandle>::Failure(admission.ErrorValue());
            auto record = std::make_shared<GameplayNavigationRecord>();
            record->storage = state_->allocateResult();
            record->cancellation = CancellationSource{request.cancellation};
            if (request.operation == GameplayNavigationOperation::Path) {
                auto accepted = state_->SubmitPath(request, record->cancellation.Token());
                if (accepted.HasError())
                    return Result<GameplayNavigationHandle>::Failure(accepted.ErrorValue());
                record->query = accepted.Value();
            } else {
                auto accepted = state_->SubmitMovement(request, record->cancellation.Token());
                if (accepted.HasError())
                    return Result<GameplayNavigationHandle>::Failure(accepted.ErrorValue());
                record->movement = accepted.Value();
            }
            state_->records.push_back(record);
            return Result<GameplayNavigationHandle>::Success(GameplayNavigationHandle{state_, std::move(record)});
        } catch (const std::bad_alloc &) {
            return Result<GameplayNavigationHandle>::Failure(MakeError(Navigation::NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc GameplayNavigationContext::IsUsable */
    bool GameplayNavigationContext::IsUsable() const noexcept {
        return state_->thread == std::this_thread::get_id() && !state_->revoked;
    }

    /** @copydoc GameplayNavigationContext::Revoke */
    void GameplayNavigationContext::Revoke() const noexcept {
        if (state_->revoked)
            return;
        state_->revoked = true;
        for (const auto &record : state_->records) {
            record->cancellation.RequestCancellation();
            if (record->query)
                static_cast<void>(state_->coordinator->Cancel(*record->query));
            if (record->movement)
                static_cast<void>(state_->movement->Cancel(*record->movement));
        }
        state_->current.reset();
    }
}  // namespace Horo::Gameplay
