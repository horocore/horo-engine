#include "EditorVulkanXrViewportAccess.h"
#include "EditorXrTrackingRoot.h"
#include "Horo/Foundation/Assertions.h"
#include "OpenXRPlayHost.h"

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
            return host_->ValidateScene(*state_);
        }

        void Publish() noexcept override {
            if (retirement_) {
                if (state_)
                    host_->RetireScene(state_);
                return;
            }
            state_->published = true;
            host_->input_.admitted = true;
        }

        void Shutdown() noexcept override {
            if (!retirement_)
                host_->RetireScene(state_);
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
            auto prepared = host_->PrepareScene(definition, scene);
            if (prepared.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(prepared.ErrorValue());
            auto state = std::move(prepared).Value();
            try {
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(std::make_unique<SceneCandidate>(*host_, state));
            } catch (...) {
                host_->RetireScene(state);
                throw;  // Preserve the original allocation/provider exception; physical debt stays with the App.
            }
        }

        Result<std::unique_ptr<Runtime::SceneActivationCandidate>> PrepareRetirement(const Runtime::RuntimeSceneView &scene) override {
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
        auto updated = scene_->geometry->Update(scene);
        if (updated.HasError()) {
            RetireScene(scene_);
            return Result<void>::Failure(updated.ErrorValue());
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::PrepareScene */
    Result<std::shared_ptr<OpenXRPlayHost::SceneState>> OpenXRPlayHost::PrepareScene(const Runtime::RuntimeSceneDefinition &definition,
                                                                                     const Runtime::RuntimeSceneView candidate) {
        using Output = Result<std::shared_ptr<SceneState>>;
        if (auto lane = Lane(); lane.HasError())
            return Output::Failure(lane.ErrorValue());
        if (scene_ || !input_.port || !input_.geometry || !input_.router || candidate.DefinitionRevision() != definition.Revision())
            return Output::Failure(MakeError(XRErrors::OperationIncompatible));
        const Operation operation{*this};
        auto geometry = input_.geometry->Capture(definition.Revision(), candidate);
        if (geometry.HasError())
            return Output::Failure(geometry.ErrorValue());
        auto root = Editor::ResolveXrTrackingRoot(candidate, composition_.trackingRoot);
        if (root.HasError())
            return Output::Failure(root.ErrorValue());
        // This revision derives from the actual canonical Scene occurrence; no parallel Scene/origin counter is issued.
        auto origin = XRWorldOriginRevision::Create(candidate.RuntimeId().value);
        if (origin.HasError())
            return Output::Failure(origin.ErrorValue());
        input_.origin = origin.Value();
        scene_ = std::make_shared<SceneState>();
        scene_->scene = candidate.RuntimeId();
        scene_->geometry = std::move(geometry).Value();

        struct PreparationScope final {
            OpenXRPlayHost &host;
            std::shared_ptr<SceneState> state;
            bool accepted{};

            ~PreparationScope() noexcept {
                if (!accepted)
                    host.RetireScene(state);
            }
        } rollback{*this, scene_};

        const auto fail = [&](const Error &error) -> Output {
            RetireScene(scene_);
            return Output::Failure(error);
        };
        if (auto prepared = PrepareSceneResources({input_.origin, XRSpaceKind::Local, root.Value()}); prepared.HasError())
            return fail(prepared.ErrorValue());
        rollback.accepted = true;
        return Output::Success(scene_);
    }

    /** @copydoc OpenXRPlayHost::PrepareSceneResources */
    Result<void> OpenXRPlayHost::PrepareSceneResources(const NativeTrackingSpaceRequest &tracking) {
        if (auto prepared = composition_.session.PrepareInput(*input_.port, *input_.router, *composition_.bindings, tracking);
            prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        auto projection = composition_.session.PrepareProjection(composition_.frontend, composition_.viewport, *composition_.bindings);
        if (projection.HasError())
            return Result<void>::Failure(projection.ErrorValue());
        scene_->projection.emplace(std::move(projection).Value());
        const auto &facts = *scene_->projection;
        auto activated = sessions_.Activate(facts.capabilities, facts.capabilities.System(), facts.capabilities.Revision(), facts.plan);
        if (activated.HasError())
            return Result<void>::Failure(activated.ErrorValue());
        scene_->activated = true;
        if (scene_->session != activated.Value())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (auto finished = FinishFrameResources(activated.Value(), facts.capabilities); finished.HasError())
            return Result<void>::Failure(finished.ErrorValue());
        if (auto valid = ValidateScene(*scene_); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::ValidateScene */
    Result<void> OpenXRPlayHost::ValidateScene(const SceneState &scene) const {
        if (std::this_thread::get_id() != lane_ || pendingRevoke_ || pendingDetach_ || scene_.get() != &scene || scene.retiring ||
            !scene.geometry || !scene.projection || !scene.session.IsValid() || !input_.port || !input_.geometry ||
            sessions_.Snapshot().session != scene.session || !input_.coordinator || !input_.coordinator->Current())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (auto source = scene.geometry->ValidateSource(); source.HasError())
            return source;
        return native_.Validate(scene.session);
    }

    /** @copydoc OpenXRPlayHost::Prepare */
    Result<void> OpenXRPlayHost::Prepare(const XRSessionPreparation stage, const XRSessionId &candidate, const XRFeaturePlan &plan) {
        if (!scene_ || scene_->retiring || !scene_->projection || &plan != &scene_->projection->plan ||
            (scene_->session.IsValid() && scene_->session != candidate))
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        scene_->session = candidate;  // Only the canonical lifecycle supplied this identity.
        switch (stage) {
            case XRSessionPreparation::NativeSessionAndSwapchains: {
                auto activated = composition_.session.ActivateSession(candidate, scene_->projection->capabilities, plan);
                if (activated.HasError())
                    return activated;
                return PrepareFrameResources(candidate, scene_->projection->color);
            }
            case XRSessionPreparation::RendererTargets: {
                auto device = Editor::EditorVulkanXrViewportAccess::Device(composition_.viewport);
                if (device.HasError())
                    return Result<void>::Failure(device.ErrorValue());
                const Render::RenderGraphLogicalState acquired{Render::RenderGraphSynchronizationAccess::ReadWrite,
                                                               Render::RenderGraphSynchronizationOperation::ColorAttachment,
                                                               Render::RenderGraphPipelineScope::Graphics,
                                                               Render::RenderGraphTextureLayout::ColorAttachment,
                                                               device.Value().graphicsIdentity};
                auto final = acquired;
                final.scope = Render::RenderGraphPipelineScope::External;
                return Editor::EditorVulkanXrViewportAccess::Prepare(composition_.viewport, scene_->geometry->Resources(),
                                                                     scene_->projection->color, acquired, final);
            }
            case XRSessionPreparation::InputAndUi: {
                input_.coordinator = std::make_unique<XRActionBindingCoordinator>(candidate, static_cast<IXRBindingNeutralizer &>(*this));
                auto bound = composition_.session.BindInput(candidate, *input_.coordinator, *input_.router, *composition_.bindings);
                if (bound.HasError())
                    return bound;
                const auto &limits = scene_->projection->capabilities.Limits();
                return tracking_.Bind(candidate, {limits.maximumDevices, 5, limits.maximumActions}, input_.origin);
            }
            case XRSessionPreparation::PlatformLoader:
            case XRSessionPreparation::InstanceSystem:
            case XRSessionPreparation::RendererCompatibility:
            case XRSessionPreparation::CapabilitiesAndSpaces:
                return scene_->geometry->ValidateSource();
            case XRSessionPreparation::Count:
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

    /** @copydoc OpenXRPlayHost::RetireScene */
    void OpenXRPlayHost::RetireScene(const std::shared_ptr<SceneState> &scene) noexcept {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == lane_, "XR Scene retirement requires the owner lane");
        if (scene_ != scene)
            return;  // Delayed outgoing Shutdown cannot revoke a replacement Scene.
        Editor::EditorVulkanXrViewportAccess::RevokeMirror(composition_.viewport);
        scene->retiring = true;
        scene->published = false;
        scene->geometry->Revoke();
        RevokeSource();
    }

    /** @copydoc OpenXRPlayHost::ProgressRetirement */
    Result<bool> OpenXRPlayHost::ProgressRetirement() {
        if (!scene_ || !scene_->retiring)
            return Result<bool>::Success(true);
        const auto candidate = scene_->session;
        if (!scene_->nativeRetired) {
            const auto resources = RetireFrameResources(candidate);
            if (resources.HasError() || !resources.Value())
                return resources;
            const auto retained = composition_.session.RetainedSession();
            auto retired = retained ? composition_.session.RetireSession(candidate) : composition_.session.RetirePreparation();
            if (retired.HasError())
                return Result<bool>::Failure(retired.ErrorValue());
            scene_->nativeRetired = true;
        }
        if (auto finished = FinishFrameRetirement(candidate); finished.HasError())
            return Result<bool>::Failure(finished.ErrorValue());
        retired_ = candidate;  // Actual native/graph/image cleanup succeeded, never a request/Scene Shutdown receipt.
        if (scene_->activated) {
            auto canonical = RuntimeInternal::XRSessionHostControl::Retire(sessions_, frames_, *this, candidate);
            if (canonical.HasError())
                return Result<bool>::Failure(canonical.ErrorValue());
        }
        input_.coordinator.reset();
        input_.origin = {};
        retiredScene_ = scene_->scene;
        scene_.reset();
        return Result<bool>::Success(true);
    }

}  // namespace Horo::Application::Internal
