#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveRestoreTransaction.h"

#include <algorithm>
#include <new>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Contains resolver faults with preconstructed failures while preserving the caller's rollback boundary. */
        Result<SaveRestoreReferenceContext> InvokeReferenceResolver(IStagedRestoreReferenceResolver &resolver,
                                                                    const StagedRestoreContext &context,
                                                                    const std::span<const StagedRestorePreparedParticipant> prepared,
                                                                    Result<SaveRestoreReferenceContext> allocationFailure,
                                                                    Result<SaveRestoreReferenceContext> callbackFailure) noexcept {
            static_assert(std::is_nothrow_move_constructible_v<Result<SaveRestoreReferenceContext>>);
            try {
                return resolver.Resolve(context, prepared);
            } catch (const std::bad_alloc &) {
                return std::move(allocationFailure);
            } catch (...) {
                return std::move(callbackFailure);
            }
        }
    }  // namespace

    /** @copydoc StagedRestoreTransaction::SetReferenceResolver */
    Result<void> StagedRestoreTransaction::SetReferenceResolver(std::unique_ptr<IStagedRestoreReferenceResolver> resolver) {
        if (!resolver || referenceResolver_ || state_ != StagedRestoreTransactionState::Created)
            return Result<void>::Failure(MakeError(SaveErrors::RestoreTransitionInvalid));
        referenceResolver_ = std::move(resolver);
        return Result<void>::Success();
    }

    /** @copydoc StagedRestoreTransaction::ResolveReferences */
    Result<void> StagedRestoreTransaction::ResolveReferences() {
        if (!referenceResolver_)
            return Result<void>::Success();
        try {
            std::vector<StagedRestorePreparedParticipant> prepared;
            prepared.reserve(staged_.size());
            for (std::size_t index = 0; index < staged_.size(); ++index) {
                const auto *projection = staged_[index]->PreparedState();
                if (!projection)
                    return FailPreparation(MakeError(SaveErrors::RestoreAdapterContractInvalid), StagedRestorePhase::FixupReferences,
                                           index);
                prepared.emplace_back(requirements_[index], projection);
            }
            // Prepare owned failures before foreign entry; the invocation itself cannot allocate on failure.
            auto allocationFailure = Result<SaveRestoreReferenceContext>::Failure(MakeError(SaveErrors::RestoreAllocationFailed));
            auto callbackFailure = Result<SaveRestoreReferenceContext>::Failure(MakeError(SaveErrors::RestoreAdapterContractInvalid));
            auto resolved =
                InvokeReferenceResolver(*referenceResolver_, context_, prepared, std::move(allocationFailure), std::move(callbackFailure));
            if (resolved.HasError())
                return FailPreparation(resolved.ErrorValue(), StagedRestorePhase::FixupReferences, requirements_.size());
            if (const auto generation = resolved.Value().Generation(); generation.registry != context_.registryGeneration ||
                                                                       generation.session != context_.sessionGeneration ||
                                                                       generation.candidateScene == 0)
                return FailPreparation(MakeError(SaveErrors::RestoreActivationStale), StagedRestorePhase::FixupReferences,
                                       requirements_.size());
            for (const auto &reference : resolved.Value().Results())
                if (!std::ranges::binary_search(requirements_, reference.owner, {}, &StagedRestoreParticipantRequirement::participant))
                    return FailPreparation(MakeError(SaveErrors::RestoreParticipantInvalid), StagedRestorePhase::FixupReferences,
                                           requirements_.size());
            references_ = std::move(resolved).Value();
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return FailPreparation(MakeError(SaveErrors::RestoreAllocationFailed), StagedRestorePhase::FixupReferences,
                                   requirements_.size());
        }
    }

}  // namespace Horo::Runtime
