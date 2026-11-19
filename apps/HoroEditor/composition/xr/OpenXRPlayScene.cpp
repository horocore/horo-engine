#include "EditorVulkanXrViewportAccess.h"
#include "EditorXrTrackingRoot.h"
#include "Horo/Foundation/Assertions.h"
#include "Horo/Runtime/Render/RenderOriginScene.h"
#include "Horo/XR/XRSessionErrors.h"
#include "OpenXRPlayFrameState.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
            host_->lastReferenceChange_.reset();
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
        SceneParticipant(OpenXRPlayHost &host, Editor::EditorMountedXrGeometryCapture mounted)
            : host_(&host), mounted_(std::move(mounted)) {}

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
        /** @brief Retires an unpublished Play attempt until its candidate takes responsibility. */
        class PreparationScope final {
        public:
            PreparationScope(OpenXRPlayHost &owner, const std::shared_ptr<SceneState> &state) noexcept : owner_(owner), state_(state) {}

            PreparationScope(const PreparationScope &) = delete;
            PreparationScope &operator=(const PreparationScope &) = delete;

            ~PreparationScope() noexcept {
                if (state_)
                    state_->Retire(owner_);
            }

            void Accept() noexcept {
                state_.reset();
            }

        private:
            OpenXRPlayHost &owner_;
            std::shared_ptr<SceneState> state_;
        };

        /** @brief Prepares the exact Scene/native candidate under the existing host rollback scope. */
        [[nodiscard]] Result<std::shared_ptr<SceneState>> PrepareCurrent(const Runtime::RuntimeSceneDefinition &definition,
                                                                         const Runtime::RuntimeSceneView &candidate);
        /** @brief Prepares geometry and calibration from the same mounted World occurrence. */
        [[nodiscard]] Result<std::shared_ptr<SceneState>> PrepareMounted(const Runtime::RuntimeSceneView &candidate) const;
        /** @brief Runs the common unpublished native preparation under the existing rollback owner. */
        [[nodiscard]] Result<std::shared_ptr<SceneState>> PrepareState(std::shared_ptr<SceneState> state,
                                                                       const NativeTrackingSpaceRequest &tracking) const;
        OpenXRPlayHost *host_;
        Editor::EditorMountedXrGeometryCapture mounted_;
    };

    /** @copydoc OpenXRPlayHost::SceneGeometry::ValidateSource */
    Result<void> OpenXRPlayHost::SceneGeometry::ValidateSource() const {
        if (revoked)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (authored)
            return authored->ValidateSource();
        if (!mounted || !mountedScene.IsCurrent())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const auto origin = mounted->origin.Get();
        return origin.HasError() ? Result<void>::Failure(origin.ErrorValue()) : Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::SceneGeometry::Update */
    Result<void> OpenXRPlayHost::SceneGeometry::Update(const Runtime::RuntimeSceneView &scene) {
        if (authored) {
            const auto updated = authored->Update(scene);
            return updated.HasError() ? Result<void>::Failure(updated.ErrorValue()) : Result<void>::Success();
        }
        if (!scene.IsCurrent() || !mountedScene.SameIncarnation(scene))
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        return ValidateSource();
    }

    /** @copydoc OpenXRPlayHost::SceneGeometry::Resources */
    std::span<const Render::RenderMeshResourceView> OpenXRPlayHost::SceneGeometry::Resources() const noexcept {
        return authored ? authored->Resources() : mounted->scene.meshResources;
    }

    /** @copydoc OpenXRPlayHost::SceneGeometry::Instances */
    std::span<const Render::RenderStaticMeshInstance> OpenXRPlayHost::SceneGeometry::Instances() const noexcept {
        return authored ? authored->Instances() : mounted->scene.instances;
    }

    /** @copydoc OpenXRPlayHost::SceneGeometry::MotionObjects */
    std::span<const Render::RenderMotionObjectId> OpenXRPlayHost::SceneGeometry::MotionObjects() const noexcept {
        return authored ? authored->MotionObjects() : std::span<const Render::RenderMotionObjectId>{};
    }

    /** @copydoc OpenXRPlayHost::SceneGeometry::Revoke */
    void OpenXRPlayHost::SceneGeometry::Revoke() noexcept {
        revoked = true;
        if (authored)
            authored->Revoke();
    }

    /** @copydoc OpenXRPlayHost::MakeSceneParticipant */
    std::unique_ptr<Runtime::SceneActivationParticipant> OpenXRPlayHost::MakeSceneParticipant(
        Editor::EditorMountedXrGeometryCapture mounted) {
        return std::make_unique<SceneParticipant>(*this, std::move(mounted));
    }

    /** @copydoc OpenXRPlayHost::UpdateScene */
    Result<void> OpenXRPlayHost::UpdateScene(const Runtime::RuntimeSceneView &scene) {
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
                using Viewport = Editor::EditorVulkanXrViewportAccess;
                const auto prepare = [&](const Viewport::AuxiliaryPlan *auxiliary) {
                    return Viewport::Prepare(composition_.viewport, scene_->geometry->Resources(), scene_->projection->color, acquired,
                                             requiredFinal, WantsMotionHistory(plan), auxiliary, scene_->geometry->Instances().size());
                };
                if (const auto &writer = scene_->projection->writer) {
                    auto depthAcquired = acquired;
                    depthAcquired.operation = Render::RenderGraphSynchronizationOperation::DepthStencilAttachment;
                    depthAcquired.layout = Render::RenderGraphTextureLayout::DepthStencilAttachment;
                    auto depthFinal = depthAcquired;
                    depthFinal.scope = Render::RenderGraphPipelineScope::External;
                    const Viewport::AuxiliaryPlan auxiliary{{writer->motion, acquired, requiredFinal},
                                                            {writer->depth, depthAcquired, depthFinal},
                                                            writer->selection.eyeExtents};
                    if (prepare(&auxiliary).HasValue())
                        return Result<void>::Success();
                    // Retain native children on this candidate, but fall back to the ordinary graph.
                    scene_->projection->writer.reset();
                    if (auto retired = Viewport::Retire(composition_.viewport); retired.HasError())
                        return retired;
                }
                return prepare(nullptr);
            }
            case InputAndUi: {
                input_.coordinator = std::make_unique<XRActionBindingCoordinator>(candidate, static_cast<IXRBindingNeutralizer &>(*this));
                if (auto bound = composition_.session.BindInput(candidate, *input_.coordinator, *input_.router, *composition_.bindings);
                    bound.HasError())
                    return bound;
                const auto &limits = scene_->projection->capabilities.Limits();
                const auto hands = plan.Decision(XRCapability::HandTracking).state;
                const auto jointLimit = hands == XRFeatureDecisionState::Required || hands == XRFeatureDecisionState::OptionalEnabled
                                            ? XRHandTrackingHardLimits::MaximumJoints
                                            : 0U;
                const auto poseLimit = 5U + (jointLimit ? XRHandTrackingHardLimits::MaximumHands : 0U);
                return tracking_.Bind(candidate, {limits.maximumDevices, poseLimit, limits.maximumActions, jointLimit}, input_.origin);
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

    /** @copydoc OpenXRPlayHost::AdmitReferenceReplacement */
    Result<void> OpenXRPlayHost::AdmitReferenceReplacement() const {
        if (!lastReferenceChange_)
            return Result<void>::Success();
        const auto now = clock_.ReadRetiredSessionTime(discoveryAttempt_);
        if (now.HasError())
            return Result<void>::Failure(now.ErrorValue());
        if (now.Value() < lastReferenceChange_->effectiveTime)
            return Result<void>::Failure(
                MakeError(XRErrors::OperationUnavailable, "Wait for the runtime reference-space change before restarting XR Play."));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::SceneParticipant::PrepareCurrent */
    Result<std::shared_ptr<OpenXRPlayHost::SceneState>> OpenXRPlayHost::SceneParticipant::PrepareCurrent(
        const Runtime::RuntimeSceneDefinition &definition, const Runtime::RuntimeSceneView &candidate) {
        auto &owner = *host_;
        using Output = Result<std::shared_ptr<SceneState>>;
        if (auto lane = owner.Lane(); lane.HasError())
            return Output::Failure(lane.ErrorValue());
        if (owner.instanceLost_)
            return Output::Failure(MakeError(XRErrors::RuntimeUnavailable, "Reopen the editor to select a fresh XR instance and device."));
        if (owner.setupCleanupPending_)
            return Output::Failure(MakeError(XRSessionErrors::SetupCleanupPending));
        if (owner.scene_ || !owner.input_.port || !owner.input_.router || candidate.DefinitionRevision() != definition.Revision())
            return Output::Failure(MakeError(XRErrors::OperationIncompatible));
        const Operation operation{owner};
        if (auto ready = owner.AdmitReferenceReplacement(); ready.HasError())
            return Output::Failure(ready.ErrorValue());
        if (mounted_)
            return PrepareMounted(candidate);
        if (!owner.input_.geometry)
            return Output::Failure(MakeError(XRErrors::OperationIncompatible));
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

        auto state = std::make_shared<SceneState>();
        state->scene = sceneIdentity;
        state->geometry.emplace(SceneGeometry{.authored = std::move(geometry).Value()});
        return PrepareState(std::move(state), {origin.Value(), XRSpaceKind::Local, root.Value()});
    }

    /** @copydoc OpenXRPlayHost::SceneParticipant::PrepareMounted */
    Result<std::shared_ptr<OpenXRPlayHost::SceneState>> OpenXRPlayHost::SceneParticipant::PrepareMounted(
        const Runtime::RuntimeSceneView &candidate) const {
        using Output = Result<std::shared_ptr<SceneState>>;
        const auto root = host_->composition_.trackingRoot;
        const auto captured = mounted_(candidate, root);
        if (captured.HasError())
            return Output::Failure(captured.ErrorValue());
        const auto &[geometry, trackingRoot, originFromLocal] = captured.Value();
        if (trackingRoot != root || !geometry || !candidate.IsCurrent() ||
            geometry->scene.lightSceneDomain != candidate.RuntimeId().value || !geometry->view.IsValid())
            return Output::Failure(MakeError(XRErrors::IdentityStale));
        constexpr auto maximum = Editor::PreparedEditorXrGeometry::MaximumInstances;
        if (geometry->scene.instances.size() > maximum || geometry->scene.meshResources.size() > maximum)
            return Output::Failure(MakeError(XRErrors::CapacityExceeded));
        if (!geometry->scene.IsValid())
            return Output::Failure(MakeError(XRErrors::OperationInvalid));
        const auto origin = geometry->origin.Get();
        if (origin.HasError())
            return Output::Failure(origin.ErrorValue());
        const auto revision = XRWorldOriginRevision::Create(origin.Value().Binding().revision.Value());
        if (revision.HasError())
            return Output::Failure(revision.ErrorValue());
        constexpr float localExtent = static_cast<float>(WorldStreaming::OriginFrame::MaximumLocalHalfExtentMillimeters) / 1000.0F;
        const auto &offset = originFromLocal.translation;
        if (originFromLocal.scale != Math::Vec3{1, 1, 1} || originFromLocal.TryToMatrix().HasError() ||
            std::max({std::abs(offset.x), std::abs(offset.y), std::abs(offset.z)}) > localExtent)
            return Output::Failure(MakeError(XRErrors::PoseInvalid, "Mounted World tracking placement must be finite and rigid."));
        auto state = std::make_shared<SceneState>();
        state->scene = candidate.RuntimeId();
        state->geometry.emplace(SceneGeometry{.mounted = geometry, .mountedScene = candidate});
        return PrepareState(std::move(state), {revision.Value(), XRSpaceKind::Local, originFromLocal});
    }

    /** @copydoc OpenXRPlayHost::SceneParticipant::PrepareState */
    Result<std::shared_ptr<OpenXRPlayHost::SceneState>> OpenXRPlayHost::SceneParticipant::PrepareState(
        std::shared_ptr<SceneState> state, const NativeTrackingSpaceRequest &tracking) const {
        using Output = Result<std::shared_ptr<SceneState>>;
        auto &owner = *host_;
        owner.input_.origin = tracking.origin;
        owner.scene_ = state;
        PreparationScope rollback{owner, state};
        if (auto prepared = state->PrepareResources(owner, tracking); prepared.HasError())
            return Output::Failure(prepared.ErrorValue());
        rollback.Accept();
        return Output::Success(std::move(state));
    }

    /** @copydoc OpenXRPlayHost::SceneState::PrepareResources */
    Result<void> OpenXRPlayHost::SceneState::PrepareResources(OpenXRPlayHost &host, const NativeTrackingSpaceRequest &tracking) {
        const auto cancellation = preparationCancellation.Token();
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(XRSessionErrors::ActivationCancelled));
        const bool planeConsent = std::exchange(host.planesArmed_, false);
        const bool gazeConsent = std::exchange(host.gazeArmed_, false);
        passthroughGrant = std::exchange(host.passthroughArmed_, std::nullopt);
        if (auto prepared = host.composition_.session.PrepareInput(*host.input_.port, *host.input_.router, *host.composition_.bindings,
                                                                   tracking, gazeConsent);
            prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(XRSessionErrors::ActivationCancelled));
        host.composition_.session.SetPlaneCaptureConsent(planeConsent);
        auto preparedProjection = host.composition_.session.PrepareProjection(host.composition_.frontend, host.composition_.viewport,
                                                                              *host.composition_.bindings);
        if (preparedProjection.HasError())
            return Result<void>::Failure(preparedProjection.ErrorValue());
        projection.emplace(std::move(preparedProjection).Value());
        if (geometry->mounted)
            projection->writer.reset();  // Mounted geometry has no issued per-draw history identity.
        const auto &facts = *projection;
        auto activatedSession = host.sessions_.Activate(facts.capabilities, facts.capabilities.System(), facts.capabilities.Revision(),
                                                        facts.plan, cancellation);
        if (activatedSession.HasError())
            return Result<void>::Failure(activatedSession.ErrorValue());
        activated = true;
        if (session != activatedSession.Value())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (auto finished = host.FinishFrameResources(activatedSession.Value(), facts.capabilities); finished.HasError())
            return Result<void>::Failure(finished.ErrorValue());
        auto observedReferences = host.composition_.session.References(session);
        if (observedReferences.HasError())
            return Result<void>::Failure(observedReferences.ErrorValue());
        references = std::move(observedReferences).Value();
        auto observedWorld = host.composition_.session.WorldSpace(session);
        if (observedWorld.HasError())
            return Result<void>::Failure(observedWorld.ErrorValue());
        worldReference = std::move(observedWorld).Value();
        if (geometry->mounted) {
            if (!tracking.worldFromBase)
                return Result<void>::Failure(MakeError(XRErrors::PoseInvalid));
            if (auto calibrated = CalibrateMounted(host, *tracking.worldFromBase); calibrated.HasError())
                return calibrated;
        }
        if (auto valid = Validate(host); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::SceneState::CalibrateMounted */
    Result<void> OpenXRPlayHost::SceneState::CalibrateMounted(const OpenXRPlayHost &host, const Math::Transform &placement) {
        if (auto current = geometry->ValidateSource(); current.HasError())
            return current;
        const auto now = host.clock_.ReferenceCalibrationNow();
        if (now.HasError())
            return Result<void>::Failure(now.ErrorValue());
        const auto sample = host.composition_.session.ObserveReferenceCalibration(references.local, worldReference, now.Value());
        if (sample.HasError())
            return Result<void>::Failure(sample.ErrorValue());
        const auto &observed = sample.Value().worldFromReference;
        const auto expectedRotation = placement.rotation.TryNormalized();
        const auto observedRotation = observed.rotation.TryNormalized();
        if (expectedRotation.HasError() || observedRotation.HasError())
            return Result<void>::Failure(MakeError(XRErrors::PoseInvalid));
        const auto &a = expectedRotation.Value();
        const auto &b = observedRotation.Value();
        const Math::Vec4 expected{a.x, a.y, a.z, a.w};
        const Math::Vec4 actual{b.x, b.y, b.z, b.w};
        const float hemisphere = Math::Dot(expected, actual) < 0.0F ? -1.0F : 1.0F;
        // Allow fixed-point millimeter resolution and fp32 error in the native rigid inverse at large local offsets.
        const auto translationTolerance =
            std::max(0.001F, Math::Length(placement.translation) * 8.0F * std::numeric_limits<float>::epsilon());
        if (Math::LengthSquared(observed.translation - placement.translation) > translationTolerance * translationTolerance ||
            Math::LengthSquared(expected - actual * hemisphere) > 1.0e-8F)
            return Result<void>::Failure(
                MakeError(XRErrors::PoseInvalid, "Native reference relation disagrees with mounted World placement."));
        if (auto current = geometry->ValidateSource(); current.HasError())
            return current;
        mountedCalibration = sample.Value();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::SceneState::Validate */
    Result<void> OpenXRPlayHost::SceneState::Validate(const OpenXRPlayHost &host) const {
        if (std::this_thread::get_id() != host.lane_ || host.pendingRevoke_ || host.pendingDetach_ || host.scene_.get() != this ||
            retiring || !geometry || !projection || !session.IsValid() || !host.input_.port ||
            host.sessions_.Snapshot().session != session || !host.input_.coordinator || !host.input_.coordinator->Current())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (auto source = geometry->ValidateSource(); source.HasError())
            return source;
        if (geometry->mounted) {
            if (!mountedCalibration || mountedCalibration->reference != references.local || mountedCalibration->world != worldReference ||
                mountedCalibration->world.worldOriginRevision != host.input_.origin)
                return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        } else if (!host.input_.geometry) {
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        }
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
        host.tracking_.Close(candidate);
        host.input_.coordinator.reset();
        host.input_.origin = {};
        host.retiredScene_ = host.scene_->scene;
        host.scene_.reset();
        return Result<bool>::Success(true);
    }

}  // namespace Horo::Application::Internal
