#include "UiBindingWriteTestFixture.h"

#include <cmath>

namespace Horo::Runtime::Ui::BindingWriteTests {
    TypedAuthority::TypedAuthority(const UiBindingWriteFence &permission, std::shared_ptr<ProviderState> owner)
        : fence(permission), state(std::move(owner)) {}

    TypedAuthority::~TypedAuthority() {
        ++state->destroyed;
    }

    const UiBindingWriteFence &TypedAuthority::Fence() const noexcept {
        return fence;
    }

    bool TypedAuthority::Active() const noexcept {
        return active;
    }

    bool TypedAuthority::FenceMatches(const UiBindingWriteCommand &command) const noexcept {
        return active && command.fence == fence && command.expected == state->revision && command.fence.property < state->values.size();
    }

    std::optional<Error> TypedAuthority::ValidateValue(const UiBindingWriteCommand &command) const {
        if (const auto &current = state->values[command.fence.property]; command.value.index() != current.index())
            return MakeError(UiErrors::BindingTypeMismatch);
        if (const auto *scalar = std::get_if<double>(&command.value); scalar && (!std::isfinite(*scalar) || *scalar < 0.0 || *scalar > 1.0))
            return MakeError(UiErrors::BindingValueInvalid);
        return std::nullopt;
    }

    void TypedAuthority::ApplyReentry() {
        if (!reentrantStore)
            return;
        if (prepareReentry == Reentry::Retirement)
            reentrantStore->BeginRetirement();
        if (prepareReentry == Reentry::Shutdown)
            reentrantStore->Shutdown();
    }

    Result<UiBindingWriteDisposition> TypedAuthority::Prepare(const UiBindingWriteCommand &command) {
        ++state->prepares;
        if (!FenceMatches(command))
            return Result<UiBindingWriteDisposition>::Failure(MakeError(UiErrors::RevisionStale));
        if (const auto error = ValidateValue(command))
            return Result<UiBindingWriteDisposition>::Failure(*error);
        if (failure)
            return Result<UiBindingWriteDisposition>::Failure(*failure);
        reservation = command;
        ApplyReentry();
        return Result<UiBindingWriteDisposition>::Success(disposition);
    }

    void TypedAuthority::Commit(const UiBindingWriteCommand &command) noexcept {
        if (!FenceMatches(command) || !reservation || reservation->request != command.request ||
            reservation->operation != command.operation || reservation->source != command.source) {
            state->commitFenceValid = false;
            return;
        }
        state->values[command.fence.property] = command.value;
        state->revision = command.expected.Next().Value();
        ++state->commits;
        reservation.reset();
    }

    void TypedAuthority::Abandon(const UiBindingWriteCommand &) noexcept {
        ++state->abandons;
        reservation.reset();
        if (reentrantStore && abandonRetires)
            reentrantStore->BeginRetirement();
    }

    void TypedAuthority::Revoke() noexcept {
        active = false;
        reservation.reset();
    }
}  // namespace Horo::Runtime::Ui::BindingWriteTests
