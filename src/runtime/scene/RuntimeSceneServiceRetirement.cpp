#include "RuntimeSceneErrors.h"
#include "RuntimeScenePublicationInternal.h"

#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Retains every detached retirement through validation and abandons only this operation on failure. */
        struct PreparedRetirement final {
            PreparedRetirement() = default;
            PreparedRetirement(const PreparedRetirement &) = delete;
            PreparedRetirement &operator=(const PreparedRetirement &) = delete;
            PreparedRetirement(PreparedRetirement &&) = delete;
            PreparedRetirement &operator=(PreparedRetirement &&) = delete;

            ~PreparedRetirement() {
                ScenePublicationDetail::ShutdownCandidates(candidates);
            }

            std::vector<std::unique_ptr<SceneActivationCandidate>> candidates;
        };
    }  // namespace

    /** @copydoc RuntimeSceneService::RetireActiveScene */
    Result<void> RuntimeSceneService::RetireActiveScene(const CancellationToken &cancellation) {
        const auto admitted = [&]() {
            if (shutdownDeferred_)
                return Result<void>::Failure(MakeError(SceneErrors::ServiceShutdown));
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Scene retirement was cancelled."));
            return Result<void>::Success();
        };
        if (const auto valid = admitted(); valid.HasError())
            return valid;
        PreparedRetirement prepared;
        try {
            prepared.candidates.reserve(participants_.size());
            const auto view = active_.scene->View();
            for (const auto &participant : participants_) {
                auto candidate = participant->PrepareRetirement(view);
                if (candidate.HasError())
                    return Result<void>::Failure(candidate.ErrorValue());
                if (candidate.Value())
                    prepared.candidates.push_back(std::move(candidate).Value());
                if (const auto valid = admitted(); valid.HasError())
                    return valid;
            }
            for (const auto &candidate : prepared.candidates) {
                if (const auto valid = candidate->ValidatePublication(); valid.HasError())
                    return valid;
                if (const auto valid = admitted(); valid.HasError())
                    return valid;
            }
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Unable to retain Scene retirement candidates."));
        }
        // The final admission check above is the existing lifecycle mutation's publication cutoff.
        // Only no-fail owner publication runs before the active aggregate transfer; no provider callbacks intervene.
        for (const auto &candidate : prepared.candidates)
            candidate->Publish();
        SceneAggregate retired = std::move(active_);
        ScenePublicationDetail::ShutdownCandidates(retired.candidates);
        retired.restore.reset();
        retired.scene.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
