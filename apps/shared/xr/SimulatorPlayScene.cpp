#include "SimulatorPlayState.h"

#include <algorithm>
#include <cmath>

namespace Horo::Application::Internal {
    using namespace XR;

    namespace {
        bool Finite(const Math::Quaternion &value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
        }

        std::optional<Editor::EditorPlayXrPreview> WorldView(const Math::Transform &root, const Math::Vec3 localPosition,
                                                             const Math::Quaternion localOrientation) noexcept {
            const auto position = root.translation + root.rotation.Rotate(localPosition);
            const auto orientation = root.rotation * localOrientation;
            if (!Math::IsFinite(position) || !Finite(orientation))
                return {};
            return Editor::EditorPlayXrPreview{position, orientation};
        }
    }  // namespace

    class SimulatorPlayHost::Impl::Candidate final : public Runtime::SceneActivationCandidate {
    public:
        Candidate(Impl &owner, std::shared_ptr<SceneState> state, Runtime::SceneRuntimeId identity, bool retirement)
            : owner_(&owner), state_(std::move(state)), identity_(identity), retirement_(retirement) {}

        Result<void> ValidatePublication() const override {
            if (retirement_) {
                if (identity_.IsValid() && owner_->retired == identity_)
                    return Result<void>::Success();
                return owner_->Validate(state_);
            }
            return owner_->Validate(state_);
        }

        void Publish() noexcept override {
            if (retirement_) {
                owner_->Retire(identity_);
                return;
            }
            state_->published = true;
            owner_->admitted = true;
        }

        void Shutdown() noexcept override {
            if (!retirement_)
                owner_->Retire(identity_);
        }

    private:
        Impl *owner_;
        std::shared_ptr<SceneState> state_;
        Runtime::SceneRuntimeId identity_;
        bool retirement_;
    };

    class SimulatorPlayHost::Impl::Participant final : public Runtime::SceneActivationParticipant {
    public:
        explicit Participant(Impl &owner) : owner_(&owner) {}

        Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                           const Runtime::RuntimeSceneView &view) override {
            auto prepared = owner_->PrepareScene(definition, view);
            if (prepared.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(prepared.ErrorValue());
            auto state = std::move(prepared).Value();
            try {
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(
                    std::make_unique<Candidate>(*owner_, state, state->identity, false));
            } catch (...) {
                owner_->Retire(state->identity);
                throw;
            }
        }

        Result<std::unique_ptr<Runtime::SceneActivationCandidate>> PrepareRetirement(const Runtime::RuntimeSceneView &view) override {
            const auto identity = view.RuntimeId();
            if (owner_->retired != identity && (!owner_->scene || owner_->scene->identity != identity))
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(MakeError(XRErrors::IdentityStale));
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(
                std::make_unique<Candidate>(*owner_, owner_->scene, identity, true));
        }

    private:
        Impl *owner_;
    };

    Result<void> SimulatorPlayHost::Impl::Validate(const std::shared_ptr<SceneState> &state) const {
        if (!state || state != scene || !runtime || !input || !source || !activeInput ||
            activeInput->session != runtime->Snapshot().session.session)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        return state->geometry->ValidateSource();
    }

    bool SimulatorPlayHost::Impl::PreparationAvailable(const Runtime::RuntimeSceneDefinition &definition,
                                                       const Runtime::RuntimeSceneView &view) const noexcept {
        return !scene && input && source && view.DefinitionRevision() == definition.Revision();
    }

    bool SimulatorPlayHost::Impl::HasConsumedView(const XRSimulatorAdvance &advanced) const noexcept {
        return scene && advanced.frame.status == XRFrameStatus::Ok && advanced.frame.viewCount > 0 && advanced.frame.views[0].has_value();
    }

    Result<std::shared_ptr<SimulatorPlayHost::Impl::SceneState>> SimulatorPlayHost::Impl::PrepareScene(
        const Runtime::RuntimeSceneDefinition &definition, const Runtime::RuntimeSceneView &view) {
        using Output = Result<std::shared_ptr<SceneState>>;
        if (auto laneCheck = Lane(); laneCheck.HasError())
            return Output::Failure(laneCheck.ErrorValue());
        if (!PreparationAvailable(definition, view))
            return Output::Failure(MakeError(XRErrors::OperationIncompatible));
        Operation operation{*this};
        auto geometry = source->Capture(definition.Revision(), view);
        if (geometry.HasError())
            return Output::Failure(geometry.ErrorValue());
        const auto root = Editor::ResolveXrTrackingRoot(view, composition.trackingRoot);
        if (root.HasError())
            return Output::Failure(root.ErrorValue());
        scene = std::make_shared<SceneState>();
        scene->identity = view.RuntimeId();
        scene->root = root.Value();
        scene->geometry = std::move(geometry).Value();

        struct Rollback final {
            Impl &owner;
            Runtime::SceneRuntimeId identity;
            bool committed{};

            Rollback(Impl &value, Runtime::SceneRuntimeId scene) noexcept : owner(value), identity(scene) {}

            Rollback(const Rollback &) = delete;
            Rollback &operator=(const Rollback &) = delete;

            ~Rollback() {
                if (!committed)
                    owner.Retire(identity);
            }
        } rollback{*this, scene->identity};

        auto created = RuntimeInternal::XRSimulatorHostAccess::Create({*this, [this] {
            return Coordinator();
        }});
        if (created.HasError())
            return Output::Failure(created.ErrorValue());
        runtime = std::move(created).Value();
        Adopt();
        auto controls = runtime->Controls();
        controls.actionCount = 1;
        controls.actions[0] = {0, XRTrackedDeviceRole::LeftController, XRTrackedActionKind::Vector2, true, 0, 0};
        if (auto applied = runtime->SetControls(controls); applied.HasError())
            return Output::Failure(applied.ErrorValue());
        if (auto valid = Validate(scene); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        rollback.committed = true;
        return Output::Success(scene);
    }

    /** @copydoc Editor::IEditorPlayXrHost::MakeSceneParticipant */
    std::unique_ptr<Runtime::SceneActivationParticipant> SimulatorPlayHost::MakeSceneParticipant() {
        return std::make_unique<Impl::Participant>(*impl_);
    }

    /** @copydoc Editor::IEditorPlayXrHost::UpdateScene */
    Result<void> SimulatorPlayHost::UpdateScene(const Runtime::RuntimeSceneView &view) {
        if (auto lane = impl_->Lane(); lane.HasError())
            return lane;
        if (!impl_->admitted || !impl_->scene || impl_->scene->identity != view.RuntimeId())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (auto updated = impl_->scene->geometry->Update(view); updated.HasError()) {
            impl_->Revoke();
            return Result<void>::Failure(updated.ErrorValue());
        }
        const auto root = Editor::ResolveXrTrackingRoot(view, impl_->composition.trackingRoot);
        if (root.HasError()) {
            impl_->Revoke();
            return Result<void>::Failure(root.ErrorValue());
        }
        // A changed tracking root invalidates the previous consumed World view until the next real cutoff.
        if (root.Value().translation != impl_->scene->root.translation || root.Value().rotation != impl_->scene->root.rotation)
            impl_->preview.reset();
        impl_->scene->root = root.Value();
        return Result<void>::Success();
    }

    void SimulatorPlayHost::Impl::ObserveView(const XRSimulatorAdvance &advanced) noexcept {
        preview.reset();
        if (!HasConsumedView(advanced))
            return;
        const auto *tracking = runtime->Tracking();
        if (!tracking)
            return;
        const auto devices = tracking->Devices();
        if (const auto head = std::ranges::find(devices, XRTrackedDeviceRole::Head, &XRTrackedDeviceRecord::role);
            head == devices.end() || head->state != XRTrackedDeviceState::Tracked)
            return;
        const auto &sample = *advanced.frame.views[0];
        const auto &components = sample.Components();
        if (!components.positionMeters.value || !components.orientation.value)
            return;
        const auto current = runtime->Snapshot();
        if (sample.Session() != current.session.session)
            return;
        preview = WorldView(scene->root, *components.positionMeters.value, *components.orientation.value);
        if (!preview)
            return;
        previewSession = current.session.session;
        previewTime = current.relativeNanoseconds;
    }
}  // namespace Horo::Application::Internal
