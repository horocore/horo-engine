#include "SceneAnimationState.h"

#include <algorithm>
#include <exception>

namespace Horo::Runtime {
    namespace A = Animation;

    class SceneAnimationRuntime::State::ActivationCandidate final : public SceneActivationCandidate {
    public:
        ActivationCandidate(std::shared_ptr<State> owner, std::shared_ptr<Scope> prepared)
            : owner_(std::move(owner)), scope_(std::move(prepared)) {}

        ~ActivationCandidate() override {
            Shutdown();
        }

        Result<void> ValidatePublication() const override {
            if (const auto admission = owner_->Admit(true); admission.HasError())
                return admission;
            if (!scope_ || scope_->closed)
                return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Stale));
            return Result<void>::Success();
        }

        void Publish() noexcept override {
            if (!owner_->OnOwner() || owner_->attempting || !scope_ || scope_->closed)
                std::terminate();
            owner_->active = scope_;
        }

        void Shutdown() noexcept override {
            if (!scope_)
                return;
            if (!owner_->OnOwner() || owner_->attempting)
                std::terminate();
            scope_->Close();
            if (owner_->active == scope_)
                owner_->active.reset();
            scope_.reset();
        }

    private:
        std::shared_ptr<State> owner_;
        std::shared_ptr<Scope> scope_;
    };

    class SceneAnimationRuntime::State::Activation final : public SceneActivationParticipant {
    public:
        explicit Activation(std::shared_ptr<State> owner) : owner_(std::move(owner)) {}

        std::optional<SceneActivationOwner> RequiredOwner() const noexcept override {
            return SceneActivationOwner::Animation;
        }

        Result<std::vector<SceneAssetDependency>> DiscoverDependencies(const RuntimeSceneAssetView &asset,
                                                                       const CancellationToken &cancellation,
                                                                       std::size_t maximumReturnedDependencies) override {
            if (const auto admission = owner_->Admit(true); admission.HasError())
                return Result<std::vector<SceneAssetDependency>>::Failure(admission.ErrorValue());
            return AnimationSceneInternal::DiscoverAssets(asset, owner_->config.target, cancellation, maximumReturnedDependencies);
        }

        Result<std::unique_ptr<SceneActivationCandidate>> Prepare(const RuntimeSceneDefinition &, const RuntimeSceneView &scene) override {
            using Return = Result<std::unique_ptr<SceneActivationCandidate>>;
            try {
                auto prepared = owner_->PrepareScope(scene);
                if (prepared.HasError())
                    return Return::Failure(prepared.ErrorValue());
                return Return::Success(std::make_unique<ActivationCandidate>(owner_, std::move(prepared).Value()));
            } catch (const std::bad_alloc &) {
                return Return::Failure(MakeError(A::AnimationAssetErrors::Capacity));
            }
        }

    private:
        std::shared_ptr<State> owner_;
    };

    class SceneAnimationRuntime::State::StructuralCandidate final : public SceneStructuralCandidate {
    public:
        StructuralCandidate(std::shared_ptr<State> owner, std::shared_ptr<Scope> scope, std::unique_ptr<Set> next)
            : owner_(std::move(owner)), scope_(std::move(scope)), next_(std::move(next)), revision_(scope_->revision) {}

        Result<void> ValidatePublication() const override {
            if (const auto admission = owner_->Admit(true); admission.HasError())
                return admission;
            if (owner_->active != scope_ || scope_->closed || scope_->revision != revision_ || !next_)
                return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Stale));
            return Result<void>::Success();
        }

        void Publish() noexcept override {
            if (!owner_->OnOwner() || owner_->attempting || owner_->active != scope_ || scope_->revision != revision_ || !next_)
                std::terminate();
            // No actor generation is shared with a replacement; unchanged actors remain exact same owners.
            next_.swap(scope_->current);
            ++scope_->revision;
            for (const auto &old : next_->actors) {
                if (!old)
                    continue;
                const bool retained = std::ranges::find(scope_->current->actors, old) != scope_->current->actors.end();
                if (!retained && old->graph.Shutdown().HasError())
                    std::terminate();
            }
            published_ = true;
        }

        Result<void> AfterPublication() override {
            if (!published_)
                return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Stale));
            next_.reset();
            return Result<void>::Success();
        }

    private:
        std::shared_ptr<State> owner_;
        std::shared_ptr<Scope> scope_;
        std::unique_ptr<Set> next_;
        std::uint64_t revision_;
        bool published_{};
    };

    class SceneAnimationRuntime::State::Structural final : public SceneStructuralParticipant {
    public:
        explicit Structural(std::shared_ptr<State> owner) : owner_(std::move(owner)) {}

        SceneStructuralOwner Owner() const noexcept override {
            return SceneStructuralOwner::Animation;
        }

        Result<std::unique_ptr<SceneStructuralCandidate>> Prepare(const RuntimeSceneView &scene, std::span<const RuntimeEntityView> created,
                                                                  std::span<const EntityRef> destroyed) override {
            using Return = Result<std::unique_ptr<SceneStructuralCandidate>>;
            try {
                auto next = owner_->PrepareChange(scene, created, destroyed);
                if (next.HasError())
                    return Return::Failure(next.ErrorValue());
                return Return::Success(std::make_unique<StructuralCandidate>(owner_, owner_->active, std::move(next).Value()));
            } catch (const std::bad_alloc &) {
                return Return::Failure(MakeError(A::AnimationAssetErrors::Capacity));
            }
        }

    private:
        std::shared_ptr<State> owner_;
    };

    std::unique_ptr<SceneActivationParticipant> SceneAnimationRuntime::MakeActivationParticipant() {
        return std::make_unique<State::Activation>(state_);
    }

    std::unique_ptr<SceneStructuralParticipant> SceneAnimationRuntime::MakeStructuralParticipant() {
        return std::make_unique<State::Structural>(state_);
    }
}  // namespace Horo::Runtime
