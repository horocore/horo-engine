#include "Horo/Navigation/NavigationCharacterAdapter.h"

#include <utility>

namespace Horo::Navigation {
    struct NavigationCharacterAdapter::State final {
        NavigationVelocityPipeline *pipeline;
        Character::CharacterCapability capability;
        NavigationCharacterBinding binding;
        CharacterInput::DesiredMotionAdapter adapter;
        std::optional<NavigationCharacterMotion> pending;
    };

    /** @copydoc NavigationCharacterAdapter::NavigationCharacterAdapter */
    NavigationCharacterAdapter::NavigationCharacterAdapter() noexcept = default;
    /** @copydoc NavigationCharacterAdapter::~NavigationCharacterAdapter */
    NavigationCharacterAdapter::~NavigationCharacterAdapter() = default;
    /** @copydoc NavigationCharacterAdapter::NavigationCharacterAdapter */
    NavigationCharacterAdapter::NavigationCharacterAdapter(NavigationCharacterAdapter &&other) noexcept = default;
    /** @copydoc NavigationCharacterAdapter::operator= */
    NavigationCharacterAdapter &NavigationCharacterAdapter::operator=(NavigationCharacterAdapter &&other) noexcept = default;

    /** @copydoc NavigationCharacterAdapter::Create */
    Result<NavigationCharacterAdapter> NavigationCharacterAdapter::Create(NavigationVelocityPipeline &pipeline,
                                                                          Character::CharacterCapability capability,
                                                                          const NavigationCharacterBinding &binding) {
        using Return = Result<NavigationCharacterAdapter>;
        if (!binding.navigation.IsValid() || !binding.characterRevision ||
            binding.controller.sceneGeneration != binding.navigation.sceneGeneration.Value() ||
            binding.principal.source != CharacterInput::DesiredMotionSource::ExternalIntent)
            return Return::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        // Allocate before transferring grant lifecycle responsibility to the generic adapter.
        auto state = std::make_unique<State>(State{&pipeline, capability, binding, {}, {}});
        auto adapter = CharacterInput::DesiredMotionAdapter::Create(binding.principal, std::move(capability), binding.controller);
        if (adapter.HasError())
            return Return::Failure(adapter.ErrorValue());
        state->adapter = std::move(adapter).Value();
        NavigationCharacterAdapter result;
        result.state_ = std::move(state);
        return Return::Success(std::move(result));
    }

    /** @copydoc NavigationCharacterAdapter::Validate */
    Result<Character::CharacterTransformPublication> NavigationCharacterAdapter::Validate(
        const NavigationVelocityPublication &publication, const NavigationMovementObservation &current) const {
        using Return = Result<Character::CharacterTransformPublication>;
        if (!state_)
            return Return::Failure(MakeError(NavigationErrors::AdmissionRejected));
        if (publication.binding != state_->binding.navigation || publication.characterRevision != state_->binding.characterRevision)
            return Return::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (const auto valid = state_->pipeline->ValidatePublicationAtSafePoint(publication, current); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        const auto root = state_->capability.ControllerTransform(state_->binding.controller);
        if (root.HasError())
            return Return::Failure(root.ErrorValue());
        if (root.Value().position != current.position || root.Value().sourceTick >= publication.intent.tick)
            return Return::Failure(MakeError(NavigationErrors::StaleSnapshot));
        return root;
    }

    /** @copydoc NavigationCharacterAdapter::Prepare */
    Result<NavigationCharacterMotion> NavigationCharacterAdapter::Prepare(const NavigationVelocityPublication &publication,
                                                                          const NavigationMovementObservation &current,
                                                                          const std::uint64_t correlation) {
        using Return = Result<NavigationCharacterMotion>;
        const auto root = Validate(publication, current);
        if (root.HasError())
            return Return::Failure(root.ErrorValue());
        if (state_->pending)
            return Return::Failure(MakeError(NavigationErrors::AdmissionRejected));
        const auto frame = state_->adapter.CaptureIntent(publication.intent.tick, correlation,
                                                         {.velocityMetersPerSecond = publication.intent.desiredVelocity});
        if (frame.HasError())
            return Return::Failure(frame.ErrorValue());
        NavigationCharacterMotion result;
        result.frame_ = frame.Value();
        result.publication_ = publication;
        result.rootRevision_ = root.Value().publicationRevision;
        result.stepSeconds_ = current.stepSeconds;
        return Return::Success(result);
    }

    /** @copydoc NavigationCharacterAdapter::Submit */
    Result<CharacterInput::DesiredMotionSubmission> NavigationCharacterAdapter::Submit(const NavigationCharacterMotion &motion,
                                                                                       const NavigationMovementObservation &current) {
        using Return = Result<CharacterInput::DesiredMotionSubmission>;
        const auto root = Validate(motion.publication_, current);
        if (root.HasError())
            return Return::Failure(root.ErrorValue());
        if (state_->pending || motion.rootRevision_ != root.Value().publicationRevision || motion.stepSeconds_ != current.stepSeconds)
            return Return::Failure(MakeError(NavigationErrors::StaleSnapshot));
        const auto submitted = state_->adapter.Submit(motion.frame_);
        if (submitted.HasValue() && !submitted.Value().absent &&
            submitted.Value().admission.status == Character::CharacterCommandAdmissionStatus::Deferred)
            state_->pending = motion;
        return submitted;
    }

    /** @copydoc NavigationCharacterAdapter::ObserveFeedback */
    Result<Character::CharacterLocomotionSnapshot> NavigationCharacterAdapter::ObserveFeedback() {
        using Return = Result<Character::CharacterLocomotionSnapshot>;
        if (!state_ || !state_->pending)
            return Return::Failure(MakeError(NavigationErrors::AdmissionRejected));
        const auto snapshot = state_->capability.ControllerLocomotionSnapshot(state_->binding.controller);
        if (snapshot.HasError())
            return Return::Failure(snapshot.ErrorValue());
        const auto root = state_->capability.ControllerTransform(state_->binding.controller);
        if (root.HasError())
            return Return::Failure(root.ErrorValue());
        const auto &motion = *state_->pending;
        const auto &result = snapshot.Value();
        if (result.controller != state_->binding.controller || result.tick != motion.Tick() || result.movement.tick != motion.Tick() ||
            result.movement.sequence != motion.Correlation() || result.transform.sourceTick != motion.Tick() ||
            result.transform.publicationRevision <= motion.rootRevision_ ||
            root.Value().publicationRevision != result.transform.publicationRevision)
            return Return::Failure(MakeError(NavigationErrors::StaleSnapshot));
        using Character::CharacterCollisionFlags;
        const auto sides =
            static_cast<std::uint16_t>(result.movement.collisions) & static_cast<std::uint16_t>(CharacterCollisionFlags::Sides);
        const bool blocked = sides != 0 || result.movement.termination != Character::CharacterMovementTermination::Complete;
        const auto &publication = motion.publication_;
        if (const auto feedback = state_->pipeline->ObserveFeedback(
                {publication.intent.operation, publication.intent.path, motion.Tick(), publication.characterRevision,
                 result.movement.achievedVelocityMetersPerSecond * motion.stepSeconds_, blocked});
            feedback.HasError())
            return Return::Failure(feedback.ErrorValue());
        state_->pending.reset();
        return snapshot;
    }

    /** @copydoc NavigationCharacterAdapter::DiscardFeedback */
    void NavigationCharacterAdapter::DiscardFeedback() noexcept {
        if (state_)
            state_->pending.reset();
    }

    /** @copydoc NavigationCharacterAdapter::Shutdown */
    void NavigationCharacterAdapter::Shutdown() noexcept {
        state_.reset();
    }
}  // namespace Horo::Navigation
