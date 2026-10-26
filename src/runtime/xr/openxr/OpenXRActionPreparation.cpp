#include "host/OpenXRActionBindings.h"

#include <format>
#include <memory>
#include <new>

namespace Horo::XR::OpenXRInternal {
    /** @copydoc OpenXRActionBindings::ValidateOwner */
    Result<void> OpenXRActionBindings::ValidateOwner() const {
        if (!preparation_)
            return session_->Validate(owner_);
        const auto current = session_->BorrowPreparedSession({preparation_->preflight, preparation_->attempt});
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (current.Value().instance != native_.instance || current.Value().system != native_.system ||
            current.Value().session != native_.session || current.Value().incarnation != native_.incarnation)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::Create */
    Result<void> OpenXRActionBindings::Create(const XRSessionId &session, const NativeActionBindingRequest &request) {
        if (auto lane = ControlLane(); lane.HasError())
            return lane;
        if (owner_.IsValid() || preparation_ || sampling_ || session == attached_.owner)
            return Result<void>::Failure(
                MakeError(XRErrors::OperationIncompatible, "Retire the previous action owner and replace the attached session first."));
        const auto borrowed = [&] {
            const Operation operation{sampling_};
            return session_->Borrow(session);
        }();
        if (borrowed.HasError())
            return Result<void>::Failure(borrowed.ErrorValue());
        native_ = borrowed.Value();
        owner_ = session;
        if (const auto prepared = PrepareTransaction(request, native_.maximumActions); prepared.HasError())
            return prepared;
        if (const auto spaces = BindSpaces(session); spaces.HasError()) {
            (void)Close();
            return spaces;
        }
        active_ = true;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::PrepareAttempt */
    Result<void> OpenXRActionBindings::PrepareAttempt(const NativeSessionPreparation &preparation, const std::uint32_t maximumActions,
                                                      const NativeActionBindingRequest &request) {
        if (auto lane = ControlLane(); lane.HasError())
            return lane;
        if (owner_.IsValid() || preparation_ || sampling_ || !actions_.empty() || !sets_.empty())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const auto borrowed = [&] {
            const Operation operation{sampling_};
            return session_->BorrowPreparedSession(preparation);
        }();
        if (borrowed.HasError())
            return Result<void>::Failure(borrowed.ErrorValue());
        if (attached_.native == borrowed.Value().session && attached_.incarnation == borrowed.Value().incarnation)
            return Result<void>::Failure(
                MakeError(XRErrors::OperationIncompatible, "OpenXR action sets attach only once to the same prepared native session."));
        preparation_.emplace(preparation.preflight, preparation.activeAttempt, maximumActions);
        native_ = borrowed.Value();  // maximumActions remains zero: creation is not admission to frame work.
        auto prepared = PrepareTransaction(request, maximumActions);
        if (prepared.HasValue())
            preparation_->complete = true;
        return prepared;
    }

    /** @copydoc OpenXRActionBindings::Bind */
    Result<void> OpenXRActionBindings::Bind(const XRSessionId &session) {
        if (auto lane = ControlLane(); lane.HasError())
            return lane;
        if (!preparation_ || !preparation_->complete || active_ || sampling_ || attached_.native == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const Operation operation{sampling_};
        const auto current = session_->Borrow(session);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (current.Value().instance != native_.instance || current.Value().system != native_.system ||
            current.Value().session != native_.session || current.Value().incarnation != native_.incarnation ||
            session.system.runtime != preparation_->preflight.RuntimeGeneration())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (current.Value().maximumActions < preparation_->maximumActions)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        if (tracking_.request &&
            current.Value().maximumSpaces < tracking_.actions.size() + 2 + (tracking_.request->worldFromBase.has_value() ? 1U : 0U))
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        if (auto spaces = BindSpaces(session); spaces.HasError())
            return spaces;
        native_ = current.Value();
        owner_ = session;
        attached_.owner = session;
        preparation_.reset();
        active_ = true;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::PrepareTransaction */
    Result<void> OpenXRActionBindings::PrepareTransaction(const NativeActionBindingRequest &request, const std::uint32_t maximumActions) {
        const auto retire = [](OpenXRActionBindings *owner) {
            owner->sampling_ = false;
            (void)owner->Close();
        };
        // Foreign callbacks cannot mutate borrowed action arrays; unwind always clears the operation guard before cleanup.
        std::unique_ptr<OpenXRActionBindings, decltype(retire)> rollback{this, retire};
        sampling_ = true;
        const auto preparationFailure = [] {
            return Result<void>::Failure(
                MakeError(XRErrors::OperationUnavailable, "OpenXR action preparation failed; rollback requested."));
        };
        Result<void> prepared = Result<void>::Success();
        try {
            prepared = ValidatePosePaths(request, maximumActions);
            if (prepared.HasValue()) {
                tracking_.request = request.tracking;
                prepared = Resolve(request, native_.instance);
            }
            if (prepared.HasValue())
                prepared = Prepare(request, maximumActions);
        } catch (const std::bad_alloc &) {
            prepared = preparationFailure();
        } catch (const std::format_error &) {
            prepared = preparationFailure();
        }
        sampling_ = false;
        (void)rollback.release();
        if (prepared.HasError()) {
            const auto retired = Close();
            return retired.HasError() ? retired : prepared;
        }
        schemaVersion_ = request.schema.version;
        fallbackProfile_ = request.schema.genericFallback;
        return Result<void>::Success();
    }

}  // namespace Horo::XR::OpenXRInternal
