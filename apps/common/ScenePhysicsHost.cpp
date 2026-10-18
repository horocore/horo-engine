#include "ScenePhysicsHost.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <new>
#include <string>
#include <utility>

namespace Horo::Application::Internal {
    /** @copydoc ParseScenePhysicsHostMode */
    std::optional<Physics::PhysicsRuntimeMode> ParseScenePhysicsHostMode(const std::string_view name) noexcept {
        if (name == "null")
            return Physics::PhysicsRuntimeMode::Null;
        if (name == "canonical")
            return Physics::PhysicsRuntimeMode::Canonical;
        return std::nullopt;
    }

    /** @copydoc ScenePhysicsHostModeName */
    std::string_view ScenePhysicsHostModeName(const Physics::PhysicsRuntimeMode mode) noexcept {
        switch (mode) {
            case Physics::PhysicsRuntimeMode::Null:
                return "null";
            case Physics::PhysicsRuntimeMode::Canonical:
                return "canonical";
        }
        return {};
    }

    /** @copydoc ParseSceneHostAbsolutePath */
    Result<std::filesystem::path> ParseSceneHostAbsolutePath(const std::string_view value) {
        const auto invalid = [] {
            return Result<std::filesystem::path>::Failure(
                MakeError(Physics::PhysicsErrors::DescriptorInvalid, "Supply a valid UTF-8 absolute Scene project/cook root."));
        };
        try {
            const std::filesystem::path path{std::u8string{value.begin(), value.end()}};
            if (!path.is_absolute())
                return invalid();
            return Result<std::filesystem::path>::Success(path.lexically_normal());
        } catch (const std::filesystem::filesystem_error &) {
            return invalid();
        }
    }

    /** @brief Forwards the real candidate's metadata while retaining its sole Scene ownership. */
    class ScenePhysicsHost::Candidate final : public Runtime::SceneActivationCandidate {
    public:
        Candidate(ScenePhysicsHost &host, std::unique_ptr<Runtime::SceneActivationCandidate> candidate) noexcept
            : host_(host), candidate_(std::move(candidate)),
              physics_(static_cast<Physics::PhysicsSceneActivationCandidate *>(candidate_.get())) {}

        ~Candidate() override {
            Shutdown();
        }

        Candidate(const Candidate &) = delete;
        Candidate &operator=(const Candidate &) = delete;
        Candidate(Candidate &&) = delete;
        Candidate &operator=(Candidate &&) = delete;

        Result<void> ValidatePublication() const override {
            return candidate_->ValidatePublication();
        }

        Runtime::SceneCanonicalDatasetProjection CanonicalDatasetProjection() const noexcept override {
            return candidate_->CanonicalDatasetProjection();
        }

        std::span<const Runtime::SceneInitialPlacement> InitialPlacements() const noexcept override {
            return candidate_->InitialPlacements();
        }

        void Publish() noexcept override {
            candidate_->Publish();
            host_.published_ = physics_;
        }

        void Shutdown() noexcept override {
            // Replacement publishes the new root before retiring the old one. Old teardown
            // must never withdraw the replacement's observer.
            if (host_.published_ == physics_)
                host_.published_ = nullptr;
            candidate_->Shutdown();
        }

    private:
        ScenePhysicsHost &host_;
        std::unique_ptr<Runtime::SceneActivationCandidate> candidate_;
        Physics::PhysicsSceneActivationCandidate *physics_;
    };

    /** @brief Wraps only the concrete explicit participant constructed here; it cannot accept a foreign candidate type. */
    class ScenePhysicsHost::Participant final : public Runtime::SceneActivationParticipant {
    public:
        Participant(ScenePhysicsHost &host, Physics::PhysicsRuntime &runtime, Physics::PhysicsSceneActivationAuthority &authority,
                    Physics::PhysicsSceneActivationSettings settings)
            : host_(host), owner_(runtime, authority, std::move(settings)) {}

        ~Participant() override {
            if (host_.participant_ == this)
                host_.participant_ = nullptr;
        }

        Participant(const Participant &) = delete;
        Participant &operator=(const Participant &) = delete;
        Participant(Participant &&) = delete;
        Participant &operator=(Participant &&) = delete;

        std::unique_ptr<Runtime::SceneStructuralParticipant> MakeStructuralParticipant() {
            return owner_.MakeStructuralParticipant();
        }

        Runtime::SceneActivationPhase PreparationPhase() const noexcept override {
            return owner_.PreparationPhase();
        }

        Result<void> PrepareOwnedScene(Runtime::RuntimeScene &scene) override {
            if (const auto identity = scene.View().RuntimeId().value;
                host_.identities_.first.value == 0 || host_.identities_.last.value < host_.identities_.first.value ||
                identity < host_.identities_.first.value || identity > host_.identities_.last.value)
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::InvalidState,
                                                       "Scene identity is outside the application's reserved Physics identity range."));
            return owner_.PrepareOwnedScene(scene);
        }

        Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                           Runtime::RuntimeSceneView scene) override {
            auto candidate = owner_.Prepare(definition, scene);
            if (candidate.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(candidate.ErrorValue());
            try {
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(
                    std::make_unique<Candidate>(host_, std::move(candidate).Value()));
            } catch (const std::bad_alloc &) {
                // Construction is load-time; explicit retirement precedes propagation if wrapping
                // allocation fails. The real Physics candidate remains the unique native owner.
                if (candidate.Value())
                    candidate.Value()->Shutdown();
                throw;
            }
        }

    private:
        ScenePhysicsHost &host_;
        Physics::PhysicsSceneActivationParticipant owner_;
    };

    /** @copydoc ScenePhysicsHost::ScenePhysicsHost */
    ScenePhysicsHost::ScenePhysicsHost(const ScenePhysicsIdentityRange identities) : identities_(identities) {}

    /** @copydoc ScenePhysicsHost::MakeParticipant */
    Result<std::unique_ptr<Runtime::SceneActivationParticipant>> ScenePhysicsHost::MakeParticipant(
        Physics::PhysicsRuntime &runtime, Physics::PhysicsSceneActivationAuthority &authority,
        Physics::PhysicsSceneActivationSettings settings) {
        if (std::this_thread::get_id() != owner_)
            return Result<std::unique_ptr<Runtime::SceneActivationParticipant>>::Failure(
                MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
        if (participant_)
            return Result<std::unique_ptr<Runtime::SceneActivationParticipant>>::Failure(
                MakeError(Physics::PhysicsErrors::InvalidState, "This Scene host already has its Physics participant."));
        auto participant = std::make_unique<Participant>(*this, runtime, authority, std::move(settings));
        participant_ = participant.get();
        return Result<std::unique_ptr<Runtime::SceneActivationParticipant>>::Success(std::move(participant));
    }

    /** @copydoc ScenePhysicsHost::MakeStructuralParticipant */
    Result<std::unique_ptr<Runtime::SceneStructuralParticipant>> ScenePhysicsHost::MakeStructuralParticipant() {
        if (std::this_thread::get_id() != owner_)
            return Result<std::unique_ptr<Runtime::SceneStructuralParticipant>>::Failure(
                MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
        if (!participant_)
            return Result<std::unique_ptr<Runtime::SceneStructuralParticipant>>::Failure(
                MakeError(Physics::PhysicsErrors::InvalidState,
                          "Register the Scene host Physics participant before its structural adapter."));
        return Result<std::unique_ptr<Runtime::SceneStructuralParticipant>>::Success(participant_->MakeStructuralParticipant());
    }

    /** @copydoc ScenePhysicsHost::AdvanceFixedTick */
    Result<void> ScenePhysicsHost::AdvanceFixedTick(const Duration delta) {
        auto borrowed = Borrow();
        if (borrowed.HasError())
            return Result<void>::Failure(borrowed.ErrorValue());
        auto &scope = borrowed.Value();
        const auto publication = scope.physics.PublishedTick();
        if (publication.completedTick == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::GenerationExhausted));
        const auto tick = publication.completedTick + 1;
        if (const auto processed = scope.physics.ProcessQueryBatch(); processed.HasError())
            return processed;
        const auto current = scope.physics.PublishedTick();
        const auto &descriptor = scope.character.Descriptor();
        if (current.publicationRevision != 0) {
            if (const auto refreshed = scope.character.RefreshPhysicsSnapshot(scope.physics.Identity(), current.publicationRevision);
                refreshed.HasError())
                return refreshed;
        } else if (current.completedTick != 0 || scope.character.PublishedTick().completedTick != 0 ||
                   descriptor.sceneGeneration != scope.scene.value || descriptor.physicsWorld != scope.physics.Identity()) {
            return Result<void>::Failure(
                MakeError(Physics::PhysicsErrors::QuerySnapshotStale,
                          "Only the exact unticked Scene-owned pair may use its prepared Character query context."));
        }
        // Initial placement already bound these exact owners. Until Physics publishes its first real tick,
        // retain the prepared Character context; no synthetic Physics publication or tick is introduced.
        const auto revision = descriptor.physicsSnapshotRevision;
        Character::CharacterPhysicsQueryAdapter adapter{scope.physics};
        const auto query = adapter.Context({descriptor.sceneGeneration, descriptor.identity, descriptor.physicsWorld,
                                            descriptor.collisionFilterGeneration, descriptor.originGeneration, tick, revision});
        if (const auto advanced =
                scope.character.AdvanceFixedTick({.tick = tick, .sceneGeneration = scope.scene.value, .fixedDelta = delta, .query = query});
            advanced.HasError())
            return advanced;
        return scope.physics.AdvanceFixedTick({.simulationTick = tick, .sceneGeneration = scope.scene.value, .fixedDelta = delta});
    }

    /** @copydoc ScenePhysicsHost::HasPublishedScene */
    bool ScenePhysicsHost::HasPublishedScene() const noexcept {
        return std::this_thread::get_id() == owner_ && published_ != nullptr;
    }

    /** @copydoc ScenePhysicsHost::Borrow */
    Result<Physics::Detail::PhysicsSceneHostScope> ScenePhysicsHost::Borrow() {
        if (std::this_thread::get_id() != owner_)
            return Result<Physics::Detail::PhysicsSceneHostScope>::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
        if (!published_)
            return Result<Physics::Detail::PhysicsSceneHostScope>::Failure(
                MakeError(Physics::PhysicsErrors::InvalidState, "No Scene-owned Physics aggregate is published."));
        return Physics::Detail::PhysicsSceneHostAccess::Borrow(*published_);
    }
}  // namespace Horo::Application::Internal
