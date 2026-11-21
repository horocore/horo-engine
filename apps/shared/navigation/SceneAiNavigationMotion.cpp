#include "SceneAiNavigationMotion.h"

#include <utility>

namespace Horo::Application::Internal {
    namespace Nav = Navigation;

    /** @copydoc SceneAiNavigationMotion::SceneAiNavigationMotion */
    SceneAiNavigationMotion::SceneAiNavigationMotion(AI::AiNavigationMovementTask task, Nav::NavigationVelocityPipeline &velocity,
                                                     Nav::NavigationCharacterAdapter &character) noexcept
        : task_(std::move(task)), velocity_(velocity), character_(character) {}

    SceneAiNavigationMotion::~SceneAiNavigationMotion() {
        DiscardFeedback();
    }

    /** @copydoc SceneAiNavigationMotion::Submit */
    Result<std::optional<Nav::NavigationVelocityPublication>> SceneAiNavigationMotion::Submit(
        const Nav::NavigationMovementObservation &current, const Nav::NavigationCrowdSnapshot &capture,
        Nav::NavigationAvoidanceRequest request, Nav::INavigationCrowdBackend &solver) {
        using Publication = Result<std::optional<Nav::NavigationVelocityPublication>>;
        if (pendingTick_ != 0 || !character_.UsesPipeline(velocity_))
            return Publication::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        auto work = task_.PrepareSafeMotion(velocity_, current, capture, request);
        if (work.HasError())
            return Publication::Failure(std::move(work).ErrorValue());
        if (!work.Value())
            return Publication::Success(std::nullopt);
        auto submitted = SubmitWork(*work.Value(), current, capture, solver);
        if (submitted.HasError()) {
            (void)velocity_.CancelPending(*work.Value());
            return Publication::Failure(task_.FailMotion(std::move(submitted).ErrorValue()));
        }
        pendingTick_ = current.path.tick;
        return Publication::Success(std::move(submitted).Value());
    }

    /** @copydoc SceneAiNavigationMotion::SubmitWork */
    Result<Nav::NavigationVelocityPublication> SceneAiNavigationMotion::SubmitWork(const Nav::NavigationVelocityWork &work,
                                                                                   const Nav::NavigationMovementObservation &current,
                                                                                   const Nav::NavigationCrowdSnapshot &capture,
                                                                                   Nav::INavigationCrowdBackend &solver) {
        using Publication = Result<Nav::NavigationVelocityPublication>;
        auto sampled = solver.Solve(capture, work.request);
        if (sampled.HasError())
            return Publication::Failure(std::move(sampled).ErrorValue());
        auto safe = velocity_.Commit(work, sampled.Value(), capture, current);
        if (safe.HasError())
            return safe;
        auto motion = character_.Prepare(safe.Value(), current, current.path.tick);
        if (motion.HasError())
            return Publication::Failure(std::move(motion).ErrorValue());
        auto admitted = character_.Submit(motion.Value(), current);
        if (admitted.HasError())
            return Publication::Failure(std::move(admitted).ErrorValue());
        if (admitted.Value().absent || admitted.Value().admission.status != Character::CharacterCommandAdmissionStatus::Deferred)
            return Publication::Failure(MakeError(Nav::NavigationErrors::AdmissionRejected));
        return safe;
    }

    /** @copydoc SceneAiNavigationMotion::Observe */
    Result<void> SceneAiNavigationMotion::Observe(const std::uint64_t tick) {
        if (pendingTick_ == 0)
            return Result<void>::Success();
        if (pendingTick_ != tick)
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        auto observed = character_.ObserveFeedback();
        if (observed.HasError())
            return Result<void>::Failure(std::move(observed).ErrorValue());
        pendingTick_ = 0;
        return Result<void>::Success();
    }

    /** @copydoc SceneAiNavigationMotion::DiscardFeedback */
    void SceneAiNavigationMotion::DiscardFeedback() noexcept {
        if (pendingTick_ != 0) {
            character_.DiscardFeedback();
            pendingTick_ = 0;
        }
    }
}  // namespace Horo::Application::Internal
