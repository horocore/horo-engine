#pragma once
#include "EditorXrSceneGeometry.h"
#include "EditorXrTrackingRoot.h"
#include "Horo/Runtime/FrameScheduler.h"
#include "Horo/XR/XRInputProjection.h"
#include "SimulatorPlayHost.h"
#include "XRSimulatorHostComposition.h"

#include <thread>

namespace Horo::Application::Internal {
    struct SimulatorPlayHost::Impl final : XR::IXRSessionResources, XR::IXRBindingNeutralizer {
        struct BoundInput final {
            XR::XRSessionId session;
            std::unique_ptr<XR::XRActionBindingCoordinator> coordinator;
            std::optional<XR::XRInputProjection> projection;
        };

        struct SceneState final {
            Runtime::SceneRuntimeId identity;
            std::unique_ptr<Editor::PreparedEditorXrGeometry> geometry;
            Math::Transform root;
            bool published{};
        };
        class Participant;
        class Candidate;
        SimulatorPlayHostComposition composition;
        std::optional<Input::InputActionCollection> input;
        std::optional<Editor::EditorXrSceneSourcePort> source;
        std::shared_ptr<SceneState> scene;
        Runtime::SceneRuntimeId retired;
        std::unique_ptr<XR::XRSimulatorRuntime> runtime;
        std::unique_ptr<BoundInput> candidateInput;
        std::unique_ptr<BoundInput> activeInput;
        std::optional<Editor::EditorPlayXrPreview> preview;
        XR::XRSessionId previewSession;
        std::uint64_t previewTime{};
        Editor::EditorPlayState play{Editor::EditorPlayState::Idle};
        bool admitted{};
        bool pendingStep{};
        bool busy{};
        const std::thread::id lane{std::this_thread::get_id()};

        explicit Impl(SimulatorPlayHostComposition value) : composition(std::move(value)) {}

        [[nodiscard]] Result<void> Lane() const;
        [[nodiscard]] bool Readable() const noexcept;
        [[nodiscard]] Result<void> Current(const XR::XRSessionId &session) const;
        [[nodiscard]] Result<void> Prepare(XR::XRSessionPreparation stage, const XR::XRSessionId &candidate,
                                           const XR::XRFeaturePlan &plan) override;
        void Release(XR::XRSessionPreparation stage, const XR::XRSessionId &session) noexcept override;
        void Neutralize(const XR::XRSessionId &session, std::uint64_t revision) noexcept override;
        [[nodiscard]] const XR::XRActionBindingCoordinator *Coordinator() const noexcept;
        void Adopt() noexcept;
        void Revoke() noexcept;
        void Retire(Runtime::SceneRuntimeId identity) noexcept;
        [[nodiscard]] bool PreparationAvailable(const Runtime::RuntimeSceneDefinition &definition,
                                                const Runtime::RuntimeSceneView &view) const noexcept;
        [[nodiscard]] bool HasConsumedView(const XR::XRSimulatorAdvance &advanced) const noexcept;
        [[nodiscard]] Result<void> Validate(const std::shared_ptr<SceneState> &state) const;
        [[nodiscard]] Result<std::shared_ptr<SceneState>> PrepareScene(const Runtime::RuntimeSceneDefinition &definition,
                                                                       const Runtime::RuntimeSceneView &view);
        [[nodiscard]] Result<XR::XRSimulatorCutoff> Cutoff(const Runtime::FrameContext &context) const;
        [[nodiscard]] Result<void> Append(const Runtime::FrameContext &context, const Input::InputCollectionFrame &frame);
        void ObserveView(const XR::XRSimulatorAdvance &advanced) noexcept;
        [[nodiscard]] Result<void> ResetInteractive();
        [[nodiscard]] Result<void> QueueStep();
        [[nodiscard]] Result<void> ResumeAdmission();
        [[nodiscard]] Result<void> Apply(const Editor::XRSimulatorCommand &command);

        struct Operation final {
            Impl &owner;

            explicit Operation(Impl &value) : owner(value) {
                owner.busy = true;
            }

            ~Operation() {
                owner.busy = false;
            }
        };
    };
}  // namespace Horo::Application::Internal
