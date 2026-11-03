#pragma once

/** @file EditorPlaySaveFailurePublication.h @brief Exact original-producer failure publication for Play's private restore adapter. */
#include "Horo/Runtime/Save/SaveOperation.h"

#include <new>
#include <type_traits>
#include <utility>

namespace Horo::Application::Internal {
    /** @brief Returns the intact result normally; if its publication copy allocates unsuccessfully, moves the owned original error
     * into the same producer before rethrowing. The exceptional path never returns a moved-from error.
     * @details SaveOperation::Fail moves its owned Error through the locked terminal transition and dispatches callbacks in the
     * existing noexcept, exception-isolating dispatcher. This helper does not promise the whole public operation API is no-throw.
     */
    template <typename T> Result<T> PreservePlaySaveFailure(Runtime::SaveOperationController &operation, Result<T> result) {
        static_assert(std::is_nothrow_move_constructible_v<Error>);
        if (!result.HasError())
            return result;
        try {
            static_cast<void>(operation.Fail(result.ErrorValue(), Runtime::SaveOperationCommitOutcome::NotCommitted));
        } catch (const std::bad_alloc &) {
            static_cast<void>(operation.Fail(std::move(result).ErrorValue(), Runtime::SaveOperationCommitOutcome::NotCommitted));
            throw;
        }
        return result;
    }
}  // namespace Horo::Application::Internal
