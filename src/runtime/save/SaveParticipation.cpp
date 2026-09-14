#include "Horo/Runtime/Save/SaveParticipation.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <exception>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace Horo::Runtime {
    struct SaveParticipationDetail::State final {
        std::uint64_t generation{};
        SaveParticipationCapabilities capabilities;
        CanonicalStateParticipantRegistry *registry{};
        ISaveParticipationOperationHost *operations{};
        std::vector<SaveParticipantId> registeredParticipants;
        bool open{true};
    };

    namespace {
        template <typename T> [[nodiscard]] Result<T> LifecycleFailure() {
            return Result<T>::Failure(MakeError(SaveErrors::LifecycleUnavailable));
        }

        [[nodiscard]] bool IsSaveMode(const SavePolicyMode mode) noexcept {
            return mode >= SavePolicyMode::Manual && mode <= SavePolicyMode::Suspend;
        }

        [[nodiscard]] bool IsValidRequest(const SaveParticipationSaveRequest &request) noexcept {
            return request.slot.IsValid() && IsSaveMode(request.mode);
        }

        [[nodiscard]] bool IsValidRequest(const SaveParticipationLoadRequest &request) noexcept {
            return request.slot.IsValid();
        }

        [[nodiscard]] bool SupportsRoles(const SaveParticipationCapabilities &capabilities, const SaveParticipantRole roles) noexcept {
            return (!HasSaveParticipantRole(roles, SaveParticipantRole::Capture) || capabilities.captureParticipants) &&
                   (!HasSaveParticipantRole(roles, SaveParticipantRole::Restore) || capabilities.restoreParticipants);
        }

        template <typename Request>
        [[nodiscard]] Result<SaveOperationHandle> SubmitRequest(
            const std::shared_ptr<SaveParticipationDetail::State> &state, const std::uint64_t generation,
            const bool SaveParticipationCapabilities::*capability, const Request &request,
            Result<SaveOperationHandle> (ISaveParticipationOperationHost::*submit)(const Request &)) {
            if (state == nullptr || !state->open || state->generation != generation)
                return LifecycleFailure<SaveOperationHandle>();
            if (!(state->capabilities.*capability))
                return Result<SaveOperationHandle>::Failure(MakeError(SaveErrors::PolicyCapabilityUnsupported));
            if (!IsValidRequest(request))
                return Result<SaveOperationHandle>::Failure(MakeError(SaveErrors::OperationInvalid));
            try {
                return (state->operations->*submit)(request);
            } catch (const std::bad_alloc &) {
                return Result<SaveOperationHandle>::Failure(MakeError(SaveErrors::OperationAllocationFailed));
            } catch (const std::exception &) {
                return Result<SaveOperationHandle>::Failure(MakeError(SaveErrors::LifecycleCallbackFailed));
            } catch (...) {
                return Result<SaveOperationHandle>::Failure(MakeError(SaveErrors::LifecycleCallbackFailed));
            }
        }
    }  // namespace

    SaveParticipationClient::SaveParticipationClient(std::weak_ptr<SaveParticipationDetail::State> state,
                                                     const std::uint64_t generation) noexcept
        : state_(std::move(state)), generation_(generation) {}

    /** @copydoc SaveParticipationClient::RegisterParticipant */
    Result<SaveParticipantRegistration> SaveParticipationClient::RegisterParticipant(
        const CanonicalStateParticipantDescriptor &descriptor, std::shared_ptr<const ISaveParticipationCaptureAdapter> adapter) const {
        const auto state = state_.lock();
        if (state == nullptr || !state->open || state->generation != generation_)
            return LifecycleFailure<SaveParticipantRegistration>();
        if (!SupportsRoles(state->capabilities, descriptor.roles))
            return Result<SaveParticipantRegistration>::Failure(MakeError(SaveErrors::PolicyCapabilityUnsupported));
        try {
            state->registeredParticipants.push_back(descriptor.participant);
        } catch (const std::bad_alloc &) {
            return Result<SaveParticipantRegistration>::Failure(MakeError(SaveErrors::ParticipantRegistryAllocationFailed));
        }
        auto registration = state->registry->Register(descriptor, std::move(adapter));
        if (registration.HasError())
            state->registeredParticipants.pop_back();
        return registration;
    }

    /** @copydoc SaveParticipationClient::RequestSave */
    Result<SaveOperationHandle> SaveParticipationClient::RequestSave(const SaveParticipationSaveRequest &request) const {
        return SubmitRequest(state_.lock(), generation_, &SaveParticipationCapabilities::saveRequests, request,
                             &ISaveParticipationOperationHost::RequestSave);
    }

    /** @copydoc SaveParticipationClient::RequestLoad */
    Result<SaveOperationHandle> SaveParticipationClient::RequestLoad(const SaveParticipationLoadRequest &request) const {
        return SubmitRequest(state_.lock(), generation_, &SaveParticipationCapabilities::loadRequests, request,
                             &ISaveParticipationOperationHost::RequestLoad);
    }

    /** @copydoc SaveParticipationClient::IsOpen */
    bool SaveParticipationClient::IsOpen() const noexcept {
        const auto state = state_.lock();
        return state != nullptr && state->open && state->generation == generation_;
    }

    /** @copydoc SaveParticipationClient::Generation */
    std::uint64_t SaveParticipationClient::Generation() const noexcept {
        return generation_;
    }

    SaveParticipationHost::SaveParticipationHost(std::shared_ptr<SaveParticipationDetail::State> state) noexcept
        : state_(std::move(state)) {}

    SaveParticipationHost::~SaveParticipationHost() {
        static_cast<void>(Close());
    }

    SaveParticipationHost::SaveParticipationHost(SaveParticipationHost &&other) noexcept = default;

    SaveParticipationHost &SaveParticipationHost::operator=(SaveParticipationHost &&other) noexcept {
        if (this != &other) {
            static_cast<void>(Close());
            state_ = std::move(other.state_);
        }
        return *this;
    }

    /** @copydoc SaveParticipationHost::Create */
    Result<SaveParticipationHost> SaveParticipationHost::Create(const std::uint64_t generation,
                                                                const SaveParticipationCapabilities capabilities,
                                                                CanonicalStateParticipantRegistry &registry,
                                                                ISaveParticipationOperationHost &operations) {
        if (generation == 0 || capabilities.apiVersion != SaveParticipationApiVersion || registry.IsClosed())
            return Result<SaveParticipationHost>::Failure(MakeError(SaveErrors::LifecycleInvalid));
        try {
            auto state = std::make_shared<SaveParticipationDetail::State>();
            state->generation = generation;
            state->capabilities = capabilities;
            state->registry = &registry;
            state->operations = &operations;
            state->registeredParticipants.reserve(MaximumSaveParticipantCount);
            return Result<SaveParticipationHost>::Success(SaveParticipationHost{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Result<SaveParticipationHost>::Failure(MakeError(SaveErrors::ParticipantRegistryAllocationFailed));
        }
    }

    /** @copydoc SaveParticipationHost::Client */
    SaveParticipationClient SaveParticipationHost::Client() const noexcept {
        return state_ == nullptr ? SaveParticipationClient{} : SaveParticipationClient{state_, state_->generation};
    }

    /** @copydoc SaveParticipationHost::Close */
    Result<void> SaveParticipationHost::Close() noexcept {
        if (state_ == nullptr || (!state_->open && state_->registeredParticipants.empty()))
            return Result<void>::Success();
        state_->open = false;
        if (state_->registry->IsClosed()) {
            state_->registeredParticipants.clear();
            return Result<void>::Success();
        }
        std::optional<Error> firstError;
        for (std::size_t remaining = state_->registeredParticipants.size(); remaining > 0; --remaining) {
            auto removed = state_->registry->Unregister(state_->registeredParticipants[remaining - 1]);
            if (removed.HasError()) {
                if (!firstError.has_value())
                    firstError.emplace(std::move(removed).ErrorValue());
            } else {
                state_->registeredParticipants.erase(state_->registeredParticipants.begin() + static_cast<std::ptrdiff_t>(remaining - 1));
            }
        }
        return firstError.has_value() ? Result<void>::Failure(std::move(*firstError)) : Result<void>::Success();
    }

    /** @copydoc SaveParticipationHost::IsOpen */
    bool SaveParticipationHost::IsOpen() const noexcept {
        return state_ != nullptr && state_->open;
    }
}  // namespace Horo::Runtime
