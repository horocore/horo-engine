#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiScreenStack.h"

#include <type_traits>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
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
    UiRouteOperationRequest UiRouteOperationRequest::Push(const UiRouteId route, const UiRouteStackGuard &guard) {
        return {UiRouteOperationKind::Push, route, guard};
    }

    /** @copydoc UiRouteOperationRequest::Pop */
    UiRouteOperationRequest UiRouteOperationRequest::Pop(const UiRouteStackGuard &guard) {
        return {UiRouteOperationKind::Pop, std::nullopt, guard};
    }

    /** @copydoc UiRouteOperationRequest::Replace */
    UiRouteOperationRequest UiRouteOperationRequest::Replace(const UiRouteId route, const UiRouteStackGuard &guard) {
        return {UiRouteOperationKind::Replace, route, guard};
    }

    /** @copydoc UiRouteOperationRequest::ReplaceTop */
    UiRouteOperationRequest UiRouteOperationRequest::ReplaceTop(const UiRouteId route, const UiRouteStackGuard &guard) {
        return Replace(route, guard);
    }

    /** @copydoc UiRouteOperationRequest::Back */
    UiRouteOperationRequest UiRouteOperationRequest::Back(const UiRouteStackGuard &guard) {
        return {UiRouteOperationKind::Back, std::nullopt, guard};
    }

    /** @copydoc UiRouteOperationRequest::Clear */
    UiRouteOperationRequest UiRouteOperationRequest::Clear(const UiRouteStackGuard &guard) {
        return {UiRouteOperationKind::Clear, std::nullopt, guard};
    }

    /** @copydoc UiRouteOperationRequest::Reset */
    UiRouteOperationRequest UiRouteOperationRequest::Reset(const UiRouteStackGuard &guard) {
        return Clear(guard);
    }

    /** @copydoc UiRouteOperationRequest::Navigate */
    UiRouteOperationRequest UiRouteOperationRequest::Navigate(const UiRouteId route, const UiRouteStackGuard &guard) {
        return {UiRouteOperationKind::Navigate, route, guard};
    }

    /** @copydoc UiRouteOperationRequest::Validate */
    Result<void> UiRouteOperationRequest::Validate() const {
        if (!IsKnown(kind, UiRouteOperationKind::Count))
            return Failure(UiErrors::RouteOperationInvalid);
        if (const bool needsRoute =
                kind == UiRouteOperationKind::Push || kind == UiRouteOperationKind::Replace || kind == UiRouteOperationKind::Navigate;
            needsRoute != route.has_value() || (route.has_value() && !route->IsValid()))
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

}  // namespace Horo::Runtime::Ui
