#pragma once

/** @file SourceReviewOverlay.h @brief Bounded source-review projection for one exact document publication. */
#include "Horo/Editor/ProposedChangeSet.h"

namespace Horo::Editor {
    enum class SourceReviewKind : std::uint8_t {
        Added,
        Removed,
        Modified
    };

    /** @brief Display range in original UTF-8 bytes and bounded before/after tooltip text. */
    struct SourceReviewChange final {
        std::size_t begin{};
        std::size_t end{};
        SourceReviewKind kind{};
        std::string description;
    };

    /** @brief Detached visual review of one operation, bound to an immutable source lease and proposal digest. */
    class SourceReviewOverlay final {
    public:
        /** @brief Builds a projection from the exact proposal and its planned operation.
         * @param proposal Reviewed proposal. @param plan Plan bound to that proposal.
         * @param operationIndex Selected canonical file operation.
         * @return Review for a current source-backed preview, or a typed failure or conflict.
         */
        static Result<SourceReviewOverlay> Capture(const Proposals::ChangeSet &proposal, const Proposals::WorkspaceEditPlan &plan,
                                                   std::size_t operationIndex);

        /** @brief Returns the source publication used to derive this overlay. */
        const SourceDocumentSnapshot &Source() const noexcept {
            return source_;
        }

        /** @brief Returns the exact reviewed proposal commitment. */
        Sha256Digest Proposal() const noexcept {
            return proposal_;
        }

        /** @brief Returns changes in original source order. */
        std::span<const SourceReviewChange> Changes() const noexcept {
            return changes_;
        }

    private:
        SourceDocumentSnapshot source_;
        Sha256Digest proposal_;
        std::vector<SourceReviewChange> changes_;
    };
}  // namespace Horo::Editor
