#include "EditorVulkanXrViewportAccess.h"
#include "EditorXrTrackingRoot.h"
#include "Horo/Foundation/Assertions.h"
#include "OpenXRPlayFrameState.h"

namespace Horo::Application::Internal {
    using namespace XR;
    using namespace XR::OpenXRInternal;

    class OpenXRPlayHost::SceneCandidate final : public Runtime::SceneActivationCandidate {
    public:
        SceneCandidate(OpenXRPlayHost &host, std::shared_ptr<SceneState> state, const bool retirement = false)
            : host_(&host), state_(std::move(state)), retirement_(retirement) {}

        SceneCandidate(OpenXRPlayHost &host, const Runtime::SceneRuntimeId retired) : host_(&host), retired_(retired), retirement_(true) {}

        Result<void> ValidatePublication() const override {
            if (retirement_) {
                if (!state_)
                    return retired_.IsValid() && host_->retiredScene_ == retired_
                               ? Result<void>::Success()
                               : Result<void>::Failure(MakeError(XRErrors::IdentityStale));
                if (host_->retiredScene_ == state_->scene && state_->retiring)
                    return Result<void>::Success();
                if (host_->scene_ != state_ || (!state_->published && !state_->retiring))
                    return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
                return Result<void>::Success();
            }
            return state_->Validate(*host_);
        }

        void Publish() noexcept override {
            if (retirement_) {
                if (state_)
                    state_->Retire(*host_);
                return;
            }
            state_->published = true;
            host_->input_.admitted = true;
        }

        void Shutdown() noexcept override {
            if (!retirement_)
                state_->Retire(*host_);
        }

    private:
        OpenXRPlayHost *host_;
        std::shared_ptr<SceneState> state_;
        Runtime::SceneRuntimeId retired_;
        bool retirement_;
    };

    class OpenXRPlayHost::SceneParticipant final : public Runtime::SceneActivationParticipant {
    public:
        explicit SceneParticipant(OpenXRPlayHost &host) : host_(&host) {}

        Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                           const Runtime::RuntimeSceneView &scene) override {
            auto prepared = PrepareCurrent(definition, scene);
            if (prepared.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(prepared.ErrorValue());
            auto state = std::move(prepared).Value();
            try {
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(std::make_unique<SceneCandidate>(*host_, state));
            } catch (...) {
                state->Retire(*host_);
                throw;  // Preserve the original allocation/provider exception; physical debt stays with the App.
            }
        }

        Result<std::unique_ptr<Runtime::SceneActivationCandidate>> PrepareRetirement(const Runtime::RuntimeSceneView &scene) override {
            // Capture the immutable identity before candidate allocation; no Scene borrow escapes this call.
            const auto identity = scene.RuntimeId();
            if (host_->retiredScene_ == identity && identity.IsValid())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(
                    std::make_unique<SceneCandidate>(*host_, identity));
            if (!host_->scene_ || host_->scene_->scene != identity)
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(MakeError(XRErrors::IdentityStale));
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(
                std::make_unique<SceneCandidate>(*host_, host_->scene_, true));
        }

    private:
        /** @brief Prepares the exact Scene/native candidate under the existing host rollback scope. */
        [[nodiscard]] Result<std::shared_ptr<SceneState>> PrepareCurrent(const Runtime::RuntimeSceneDefinition &definition,
                                                                         const Runtime::RuntimeSceneView &candidate);
        OpenXRPlayHost *host_;
    };

    /** @copydoc OpenXRPlayHost::MakeSceneParticipant */
    std::unique_ptr<Runtime::SceneActivationParticipant> OpenXRPlayHost::MakeSceneParticipant() {
        return std::make_unique<SceneParticipant>(*this);
    }

    /** @copydoc OpenXRPlayHost::UpdateScene */
    Result<void> OpenXRPlayHost::UpdateScene(const Runtime::RuntimeSceneView scene) {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (!scene_ || !scene_->published || scene_->retiring || scene_->scene != scene.RuntimeId())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (auto updated = scene_->geometry->Update(scene); updated.HasError()) {
            scene_->Retire(*this);
            return Result<void>::Failure(updated.ErrorValue());
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::Prepare */
    Result<void> OpenXRPlayHost::Prepare(const XRSessionPreparation stage, const XRSessionId &candidate, const XRFeaturePlan &plan) {
        if (!scene_ || scene_->retiring || !scene_->projection || &plan != &scene_->projection->plan ||
            (scene_->session.IsValid() && scene_->session != candidate))
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        scene_->session = candidate;  // Only the canonical lifecycle supplied this identity.
        switch (stage) {
            using enum Horo::XR::XRSessionPreparation;
            case NativeSessionAndSwapchains: {
                if (auto activated = composition_.session.ActivateSession(candidate, scene_->projection->capabilities, plan);
                    activated.HasError())
                    return activated;
                return PrepareFrameResources(candidate, scene_->projection->color);
            }
            case RendererTargets: {
                auto device = Editor::EditorVulkanXrViewportAccess::Device(composition_.viewport);
                if (device.HasError())
                    return Result<void>::Failure(device.ErrorValue());
                const Render::RenderGraphLogicalState acquired{Render::RenderGraphSynchronizationAccess::ReadWrite,
                                                               Render::RenderGraphSynchronizationOperation::ColorAttachment,
                                                               Render::RenderGraphPipelineScope::Graphics,
                                                               Render::RenderGraphTextureLayout::ColorAttachment,
                                                               device.Value().graphicsIdentity};
                auto requiredFinal = acquired;
                requiredFinal.scope = Render::RenderGraphPipelineScope::External;
                return Editor::EditorVulkanXrViewportAccess::Prepare(composition_.viewport, scene_->geometry->Resources(),
                                                                     scene_->projection->color, acquired, requiredFinal);
            }
            case InputAndUi: {
                input_.coordinator = std::make_unique<XRActionBindingCoordinator>(candidate, static_cast<IXRBindingNeutralizer &>(*this));
                if (auto bound = composition_.session.BindInput(candidate, *input_.coordinator, *input_.router, *composition_.bindings);
                    bound.HasError())
                    return bound;
                const auto &limits = scene_->projection->capabilities.Limits();
                return tracking_.Bind(candidate, {limits.maximumDevices, 5, limits.maximumActions}, input_.origin);
            }
            case PlatformLoader:
            case InstanceSystem:
            case RendererCompatibility:
            case CapabilitiesAndSpaces:
                return scene_->geometry->ValidateSource();
            case Count:
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        }
        return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
    }

    /** @copydoc OpenXRPlayHost::Release */
    void OpenXRPlayHost::Release(const XRSessionPreparation, const XRSessionId &session) noexcept {
        // A failed activation transfers its partial native debt to the SAME App owner. Never destroy native children
        // or outgoing Scene storage from this noexcept lifecycle callback, and never infer quiescence from rollback.
        if (scene_ && scene_->session == session) {
            scene_->retiring = true;
            scene_->geometry->Revoke();
            input_.admitted = false;
            tracking_.Revoke();
        }
    }

    /** @copydoc OpenXRPlayHost::Neutralize */
    void OpenXRPlayHost::Neutralize(const XRSessionId &, std::uint64_t) noexcept {
        tracking_.Revoke();  // Input's ONE actual cutoff owns the neutral commit; callbacks cannot commit Gameplay input.
    }

    /** @copydoc OpenXRPlayHost::SceneParticipant::PrepareCurrent */
    Result<std::shared_ptr<OpenXRPlayHost::SceneState>> OpenXRPlayHost::SceneParticipant::PrepareCurrent(
        const Runtime::RuntimeSceneDefinition &definition, const Runtime::RuntimeSceneView &candidate) {
        auto &owner = *host_;
        using Output = Result<std::shared_ptr<SceneState>>;
        if (auto lane = owner.Lane(); lane.HasError())
            return Output::Failure(lane.ErrorValue());
        if (owner.scene_ || !owner.input_.port || !owner.input_.geometry || !owner.input_.router ||
            candidate.DefinitionRevision() != definition.Revision())
            return Output::Failure(MakeError(XRErrors::OperationIncompatible));
        const Operation operation{owner};
        auto geometry = owner.input_.geometry->Capture(definition.Revision(), candidate);
        if (geometry.HasError())
            return Output::Failure(geometry.ErrorValue());
        auto root = Editor::ResolveXrTrackingRoot(candidate, owner.composition_.trackingRoot);
        if (root.HasError())
            return Output::Failure(root.ErrorValue());
        // This revision derives from the actual canonical Scene occurrence; no parallel Scene/origin counter is issued.
        const auto sceneIdentity = candidate.RuntimeId();
        auto origin = XRWorldOriginRevision::Create(sceneIdentity.value);
        if (origin.HasError())
            return Output::Failure(origin.ErrorValue());
        owner.input_.origin = origin.Value();
        owner.scene_ = std::make_shared<SceneState>();
        owner.scene_->scene = sceneIdentity;
        owner.scene_->geometry = std::move(geometry).Value();

        struct PreparationScope final {
            PreparationScope(OpenXRPlayHost &owner, std::shared_ptr<SceneState> prepared) noexcept
                : host(owner), state(std::move(prepared)) {}

            PreparationScope(const PreparationScope &) = delete;
            PreparationScope &operator=(const PreparationScope &) = delete;

            OpenXRPlayHost &host;
            std::shared_ptr<SceneState> state;
            bool accepted{};

            ~PreparationScope() noexcept {
                if (!accepted)
                    state->Retire(host);
            }
        };

        PreparationScope rollback{owner, owner.scene_};

        const auto fail = [&owner](const Error &error) {
            owner.scene_->Retire(owner);
            return Output::Failure(error);
        };
        // All Scene/view reads and geometry leases are captured before native/host preparation can call foreign code.
        if (auto prepared = owner.scene_->PrepareResources(owner, {owner.input_.origin, XRSpaceKind::Local, root.Value()});
            prepared.HasError())
            return fail(prepared.ErrorValue());
        rollback.accepted = true;
        return Output::Success(owner.scene_);
    }

    /** @copydoc OpenXRPlayHost::SceneState::PrepareResources */
    Result<void> OpenXRPlayHost::SceneState::PrepareResources(OpenXRPlayHost &host, const NativeTrackingSpaceRequest &tracking) {
        if (auto prepared =
                host.composition_.session.PrepareInput(*host.input_.port, *host.input_.router, *host.composition_.bindings, tracking);
            prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        auto projection = host.composition_.session.PrepareProjection(host.composition_.frontend, host.composition_.viewport,
                                                                      *host.composition_.bindings);
        if (projection.HasError())
            return Result<void>::Failure(projection.ErrorValue());
        host.scene_->projection.emplace(std::move(projection).Value());
        const auto &facts = *host.scene_->projection;
        auto activated =
            host.sessions_.Activate(facts.capabilities, facts.capabilities.System(), facts.capabilities.Revision(), facts.plan);
        if (activated.HasError())
            return Result<void>::Failure(activated.ErrorValue());
        host.scene_->activated = true;
        if (host.scene_->session != activated.Value())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (auto finished = host.FinishFrameResources(activated.Value(), facts.capabilities); finished.HasError())
            return Result<void>::Failure(finished.ErrorValue());
        if (auto valid = Validate(host); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::SceneState::Validate */
    Result<void> OpenXRPlayHost::SceneState::Validate(const OpenXRPlayHost &host) const {
        if (std::this_thread::get_id() != host.lane_ || host.pendingRevoke_ || host.pendingDetach_ || host.scene_.get() != this ||
            retiring || !geometry || !projection || !session.IsValid() || !host.input_.port || !host.input_.geometry ||
            host.sessions_.Snapshot().session != session || !host.input_.coordinator || !host.input_.coordinator->Current())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (auto source = geometry->ValidateSource(); source.HasError())
            return source;
        return host.native_.Validate(session);
    }

    /** @copydoc OpenXRPlayHost::SceneState::Retire */
    void OpenXRPlayHost::SceneState::Retire(OpenXRPlayHost &host) noexcept {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == host.lane_, "XR Scene retirement requires the owner lane");
        if (host.scene_.get() != this)
            return;  // Delayed outgoing Shutdown cannot revoke a replacement Scene.
        Editor::EditorVulkanXrViewportAccess::RevokeMirror(host.composition_.viewport);
        this->retiring = true;
        this->published = false;
        this->geometry->Revoke();
        host.RevokeSource();
    }

    /** @copydoc OpenXRPlayHost::SceneState::ProgressRetirement */
    Result<bool> OpenXRPlayHost::SceneState::ProgressRetirement(OpenXRPlayHost &host) {
        if (!host.scene_ || !host.scene_->retiring)
            return Result<bool>::Success(true);
        const auto candidate = host.scene_->session;
        if (!host.scene_->nativeRetired) {
            if (const auto resources = FrameState::RetireResources(host, candidate); resources.HasError() || !resources.Value())
                return resources;
            const auto retained = host.composition_.session.RetainedSession();
            if (auto retired =
                    retained ? host.composition_.session.RetireSession(candidate) : host.composition_.session.RetirePreparation();
                retired.HasError())
                return Result<bool>::Failure(retired.ErrorValue());
            host.scene_->nativeRetired = true;
        }
        if (auto finished = host.FinishFrameRetirement(candidate); finished.HasError())
            return Result<bool>::Failure(finished.ErrorValue());
        host.retired_ = candidate;  // Actual native/graph/image cleanup succeeded, never a request/Scene Shutdown receipt.
        if (host.scene_->activated) {
            auto canonical = RuntimeInternal::XRSessionHostControl::Retire(host.sessions_, host.frames_, host, candidate);
            if (canonical.HasError())
                return Result<bool>::Failure(canonical.ErrorValue());
        }
        host.input_.coordinator.reset();
        host.input_.origin = {};
        host.retiredScene_ = host.scene_->scene;
        host.scene_.reset();
        return Result<bool>::Success(true);
    }

}  // namespace Horo::Application::Internal
