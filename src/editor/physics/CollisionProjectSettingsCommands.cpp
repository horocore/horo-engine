#include "CollisionProjectSettingsSessionState.h"

#include <algorithm>
#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Preserves the blocking meaning; the complete owned review remains available for presentation. */
        Error Blocked(const CollisionSettingsReview &review) {
            for (const auto &impact : review.Impacts()) {
                if (!impact.blocksPublication)
                    continue;
                const auto reason = impact.reason == CollisionImpactReason::ActiveGeneration ? CollisionSettingsFailure::Active
                                    : impact.unsupported                                     ? CollisionSettingsFailure::Unsupported
                                                                                             : CollisionSettingsFailure::InUse;
                return MakeError(CollisionSettingsError(reason));
            }
            return MakeError(CollisionSettingsError(CollisionSettingsFailure::Stale));
        }
    }  // namespace

    /** @copydoc CollisionSettingsReview::CollisionSettingsReview */
    CollisionSettingsReview::CollisionSettingsReview(std::shared_ptr<const State> state) : state_(std::move(state)) {}

    /** @copydoc CollisionSettingsReview::Impacts */
    std::span<const CollisionReferenceImpact> CollisionSettingsReview::Impacts() const noexcept {
        return state_->impacts;
    }

    /** @copydoc CollisionSettingsReview::CanCommit */
    bool CollisionSettingsReview::CanCommit() const noexcept {
        return state_->allowed && !state_->opening.expired();
    }

    /** @copydoc CollisionProjectSettingsSession::Preview */
    Result<CollisionSettingsReview> CollisionProjectSettingsSession::Preview(Application::ProjectCollisionSettingsInput input) const {
        using Return = Result<CollisionSettingsReview>;
        try {
            if (const auto admitted = state_->Admit(); admitted.HasError())
                return Return::Failure(admitted.ErrorValue());
            if (input.projectId != state_->projectId || state_->saved.settings->Input().revision == UINT64_MAX)
                return Return::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Invalid)));
            input.revision = state_->saved.settings->Input().revision + 1;
            auto candidate = Application::ProjectCollisionSettings::Create(std::move(input));
            if (candidate.HasError())
                return Return::Failure(candidate.ErrorValue());
            auto owned = std::make_shared<const Application::ProjectCollisionSettings>(std::move(candidate).Value());
            auto review = state_->Review(std::move(owned));
            if (review.HasError())
                return Return::Failure(review.ErrorValue());
            return Return::Success(CollisionSettingsReview{std::move(review).Value()});
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
        }
    }

    /** @copydoc CollisionProjectSettingsSession::Commit */
    Result<void> CollisionProjectSettingsSession::Commit(const CollisionSettingsReview &review) {
        try {
            if (const auto admitted = state_->Admit(); admitted.HasError())
                return admitted;
            if (!review.CanCommit())
                return Result<void>::Failure(Blocked(review));
            if (state_->revision == UINT64_MAX)
                return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
            std::vector<std::shared_ptr<const Application::ProjectCollisionSettings>> history;
            history.reserve(state_->cursor + 2);
            history.assign(state_->history.begin(), state_->history.begin() + state_->cursor + 1);
            history.push_back(review.state_->candidate);
            if (const auto fresh = state_->Fresh(*review.state_); fresh.HasError())
                return fresh;
            state_->draft = review.state_->candidate;
            state_->history.swap(history);
            ++state_->cursor;
            ++state_->revision;
            state_->dirty = true;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
        }
    }

    /** @copydoc CollisionProjectSettingsSession::Replay */
    Result<void> CollisionProjectSettingsSession::Replay(const bool redo) {
        try {
            if (const auto admitted = state_->Admit(); admitted.HasError())
                return admitted;
            if ((redo && state_->cursor + 1 >= state_->history.size()) || (!redo && state_->cursor == 0) || state_->revision == UINT64_MAX)
                return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Invalid)));
            const auto target = redo ? state_->cursor + 1 : state_->cursor - 1;
            auto review = Preview(state_->history[target]->Input());
            if (review.HasError())
                return Result<void>::Failure(review.ErrorValue());
            if (!review.Value().CanCommit())
                return Result<void>::Failure(Blocked(review.Value()));
            if (const auto fresh = state_->Fresh(*review.Value().state_); fresh.HasError())
                return fresh;
            const bool clean = state_->savedDurable && state_->history[target] == state_->saved.settings;
            state_->draft = clean ? state_->saved.settings : review.Value().state_->candidate;
            state_->cursor = target;
            ++state_->revision;
            state_->dirty = !clean;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
        }
    }

    /** @copydoc CollisionProjectSettingsSession::Save */
    Result<ProjectCollisionPublication> CollisionProjectSettingsSession::Save() {
        using Return = Result<ProjectCollisionPublication>;
        try {
            if (const auto admitted = state_->Admit(); admitted.HasError())
                return Return::Failure(admitted.ErrorValue());
            if (state_->revision == UINT64_MAX)
                return Return::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
            auto review = Preview(state_->draft->Input());
            if (review.HasError())
                return Return::Failure(review.ErrorValue());
            if (!review.Value().CanCommit())
                return Return::Failure(Blocked(review.Value()));
            if (const auto fresh = state_->Fresh(*review.Value().state_); fresh.HasError())
                return Return::Failure(fresh.ErrorValue());
            auto saved = state_->store->Save(state_->saved, *review.Value().state_->candidate,
                                             CollisionPublicationSourceFence{review.Value().state_->registrySnapshot,
                                                                             review.Value().state_->sources});
            if (saved.HasError())
                return saved;
            auto receipt = std::move(saved).Value();
            // Every assignment after replacement is nonallocating; uncertainty advances source CAS but keeps dirty state.
            state_->saved = receipt.snapshot;
            state_->draft = receipt.snapshot.settings;
            state_->history[state_->cursor] = state_->draft;
            state_->savedDurable = !receipt.durabilityError.has_value();
            state_->dirty = !state_->savedDurable;
            ++state_->revision;
            return Return::Success(std::move(receipt));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
        }
    }
}  // namespace Horo::Editor
