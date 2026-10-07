#include "Horo/Runtime/Save/SaveRestoreReferenceContext.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <memory>
#include <new>
#include <type_traits>

namespace Horo::Runtime {
    namespace {
        /** @brief Checks the closed stable result envelope before retaining any owner input. */
        [[nodiscard]] bool ValidResult(const SaveRestoreReferenceResult &result) noexcept {
            using enum SaveRestoreReferenceDisposition;
            if (!result.owner.IsValid() || result.reference.value == 0 ||
                (result.disposition != Resolved && result.disposition != Remapped && result.disposition != OptionalAbsent) ||
                (result.required && result.disposition == OptionalAbsent))
                return false;
            return std::visit([]<typename Type>(const Type &target) {
                if constexpr (std::is_same_v<Type, SaveRestoreEntityTarget>)
                    return target.identity.IsValid() && target.incarnation != 0;
                else if constexpr (std::is_same_v<Type, SaveRestoreComponentTarget>)
                    return target.owner.identity.IsValid() && target.owner.incarnation != 0;
                else if constexpr (std::is_same_v<Type, SaveRestoreAssetTarget>)
                    return target.identity.IsValid();
                else if constexpr (std::is_same_v<Type, SaveRestoreServiceTarget>)
                    return target.generation != 0;
                else
                    return target.identity.IsValid() && target.schema.IsValid() && (!target.record || target.record->IsValid());
            }, result.target);
        }

        /** @brief Canonical participant-first then schema-reference order. */
        [[nodiscard]] bool ResultLess(const SaveRestoreReferenceResult &left, const SaveRestoreReferenceResult &right) noexcept {
            return left.owner < right.owner || (left.owner == right.owner && left.reference < right.reference);
        }
    }  // namespace

    /** @copydoc SaveRestoreReferenceView::Find */
    const SaveRestoreReferenceResult *SaveRestoreReferenceView::Find(const SaveRestoreReferenceId reference) const noexcept {
        const auto found = std::ranges::lower_bound(references, reference, {}, &SaveRestoreReferenceResult::reference);
        return found == references.end() || found->reference != reference ? nullptr : std::to_address(found);
    }

    /** @copydoc SaveRestoreReferenceContext::Create */
    Result<SaveRestoreReferenceContext> SaveRestoreReferenceContext::Create(const SaveRestoreReferenceGeneration generation,
                                                                            const std::span<const SaveRestoreReferenceResult> input,
                                                                            const std::size_t maximumReferences) {
        if (generation.session == 0 || generation.registry == 0 || generation.candidateScene == 0 || maximumReferences == 0 ||
            maximumReferences > 65'536 || input.size() > maximumReferences || !std::ranges::all_of(input, ValidResult))
            return Result<SaveRestoreReferenceContext>::Failure(MakeError(SaveErrors::ReferenceResolutionInvalid));
        try {
            std::vector<SaveRestoreReferenceResult> results(input.begin(), input.end());
            std::ranges::sort(results, ResultLess);
            if (std::ranges::adjacent_find(results, [](const auto &left, const auto &right) {
                return left.owner == right.owner && left.reference == right.reference;
            }) != results.end())
                return Result<SaveRestoreReferenceContext>::Failure(MakeError(SaveErrors::ReferenceResolutionInvalid));
            return Result<SaveRestoreReferenceContext>::Success(SaveRestoreReferenceContext{generation, std::move(results)});
        } catch (const std::bad_alloc &) {
            return Result<SaveRestoreReferenceContext>::Failure(MakeError(SaveErrors::RestoreAllocationFailed));
        }
    }

    /** @copydoc SaveRestoreReferenceContext::ForParticipant */
    SaveRestoreReferenceView SaveRestoreReferenceContext::ForParticipant(const SaveParticipantId &owner) const noexcept {
        const auto first = std::ranges::lower_bound(results_, owner, {}, &SaveRestoreReferenceResult::owner);
        const auto last = std::ranges::upper_bound(first, results_.end(), owner, {}, &SaveRestoreReferenceResult::owner);
        return {generation_, std::span<const SaveRestoreReferenceResult>{first, last}};
    }

    /** @copydoc SaveRestoreReferenceContext::Generation */
    SaveRestoreReferenceGeneration SaveRestoreReferenceContext::Generation() const noexcept {
        return generation_;
    }

    /** @copydoc SaveRestoreReferenceContext::Results */
    std::span<const SaveRestoreReferenceResult> SaveRestoreReferenceContext::Results() const noexcept {
        return results_;
    }

    SaveRestoreReferenceContext::SaveRestoreReferenceContext(const SaveRestoreReferenceGeneration generation,
                                                             std::vector<SaveRestoreReferenceResult> results) noexcept
        : generation_(generation), results_(std::move(results)) {}
}  // namespace Horo::Runtime
