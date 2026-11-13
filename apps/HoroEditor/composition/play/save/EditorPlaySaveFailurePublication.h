#pragma once

/** @file EditorPlaySaveFailurePublication.h @brief Exact original-producer failure publication for Play's private restore adapter. */
#include "Horo/Runtime/Save/SaveOperation.h"

#include <new>
#include <utility>

namespace Horo::Application::Internal {
    /** @brief Returns the intact result normally; if its publication copy allocates unsuccessfully, moves the owned original error
     * into the same producer before rethrowing. The exceptional path never returns a moved-from error.
     * @details The input remains alive for this synchronous call. Consuming a reference avoids a checked-STL Result move before
     * publication starts. SaveOperation::Fail assigns the owned Error into reserved terminal storage and dispatches callbacks in the
     * existing noexcept, exception-isolating dispatcher. This helper does not promise the whole public operation API is no-throw.
     */
    template <typename T> Result<T> PreservePlaySaveFailure(Runtime::SaveOperationController &operation, Result<T> &&result) {
        if (!result.HasError())
            return std::move(result);
        try {
            static_cast<void>(operation.Fail(result.ErrorValue(), Runtime::SaveOperationCommitOutcome::NotCommitted));
        } catch (const std::bad_alloc &) {
            static_cast<void>(operation.Fail(std::move(result).ErrorValue(), Runtime::SaveOperationCommitOutcome::NotCommitted));
            throw;
        }
        return std::move(result);
    }
}  // namespace Horo::Application::Internal
