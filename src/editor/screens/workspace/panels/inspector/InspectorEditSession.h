#pragma once

#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Horo::Editor {
    /** @brief Per-axis mixed-value state projected from the selected object set. */
    struct InspectorTransformMixedValues {
        std::array<bool, 3> position{};
        std::array<bool, 3> rotation{};
        std::array<bool, 3> scale{};
    };

    /** @brief Per-axis fields changed by one transform widget interaction. */
    struct InspectorTransformAxisMask {
        std::array<bool, 3> position{};
        std::array<bool, 3> rotation{};
        std::array<bool, 3> scale{};

        /** @brief Reports whether at least one transform axis is included. */
        [[nodiscard]] bool Any() const noexcept;
    };

    /** @brief UI-independent draft values owned by one Inspector edit session. */
    struct InspectorObjectDraft {
        std::optional<SceneObjectId> object;
        std::size_t selectedObjectCount{0};
        DocumentRevision revision;
        std::string name;
        std::array<float, 3> position{};
        std::array<float, 3> rotationDegrees{};
        std::array<float, 3> scale{1.0F, 1.0F, 1.0F};
        InspectorTransformMixedValues mixed;
        std::optional<Runtime::CameraComponent> camera;
        float cameraFieldOfViewDegrees{60.0F};
        std::optional<Runtime::LightComponent> light;
        float lightInnerConeDegrees{20.0F};
        float lightOuterConeDegrees{45.0F};
        std::optional<Runtime::TriggerVolumeComponent> triggerVolume;
        std::optional<Runtime::AudioSourceComponent> audioSource;
    };

    /** @brief Semantic name-widget events consumed without an ImGui dependency. */
    struct InspectorNameEdit {
        bool cancelled{false};
        bool committed{false};
    };

    /** @brief Semantic transform-widget events consumed without an ImGui dependency. */
    struct InspectorTransformEdit {
        bool changed{false};
        bool committed{false};
        bool cancelRequested{false};
        bool resetRequested{false};
        InspectorTransformAxisMask changedAxes;
    };

    /** @brief Semantic Camera-widget events consumed without an ImGui dependency. */
    struct InspectorCameraEdit {
        bool cancelRequested{false};
        bool committed{false};
        bool removeRequested{false};
    };

    /** @brief Semantic Light-widget events consumed without an ImGui dependency. */
    struct InspectorLightEdit {
        bool changed{false};
        bool committed{false};
        bool cancelRequested{false};
        bool removeRequested{false};
    };

    /** @brief Semantic TriggerVolume-widget events consumed without an ImGui dependency. */
    struct InspectorTriggerVolumeEdit {
        bool cancelRequested{false};
        bool committed{false};
        bool removeRequested{false};
    };

    /** @brief Semantic AudioSource-widget events consumed without an ImGui dependency. */
    struct InspectorAudioSourceEdit {
        bool cancelRequested{false};
        bool committed{false};
        bool removeRequested{false};
    };

    /**
     * @brief Owns Inspector draft and preview lifecycle independently from rendering.
     *
     * Widget code may update the exposed draft values. Reducer methods translate
     * semantic widget events into at most one typed workspace command.
     */
    class InspectorEditSession {
    public:
        /** @brief Returns the mutable presentation draft used by Inspector widgets. */
        [[nodiscard]] InspectorObjectDraft &Draft() noexcept;

        /** @brief Returns the current presentation draft. */
        [[nodiscard]] const InspectorObjectDraft &Draft() const noexcept;

        /**
         * @brief Reconciles selection/revision changes and refreshes the draft.
         * @return A preview-cancellation command when the previous edit session owned one.
         */
        [[nodiscard]] EditorWorkspaceViewCommandData BeginObject(const SceneObject &object, DocumentRevision revision);

        /**
         * @brief Reconciles a complete ordered selection projection.
         * @param objects All current scene-object projections.
         * @param selectedObjects Ordered selected object identities.
         * @param primary Primary selected identity, when one exists.
         * @param revision Current document revision.
         * @return A preview-cancellation command when the previous edit session owned one.
         */
        [[nodiscard]] EditorWorkspaceViewCommandData BeginSelection(std::span<const SceneObject> objects,
                                                                    std::span<const SceneObjectId> selectedObjects,
                                                                    std::optional<SceneObjectId> primary, DocumentRevision revision);

        /**
         * @brief Clears the current edit session.
         * @return A preview-cancellation command when a transient preview was active.
         */
        [[nodiscard]] EditorWorkspaceViewCommandData Clear();

        /** @brief Reduces one name edit into a typed workspace command. */
        [[nodiscard]] EditorWorkspaceViewCommandData ApplyNameEdit(const InspectorNameEdit &edit, const SceneObject &object,
                                                                   bool allowCommands);

        /** @brief Reduces one transform edit into preview, commit, or cancel. */
        [[nodiscard]] EditorWorkspaceViewCommandData ApplyTransformEdit(const InspectorTransformEdit &edit, bool allowCommands);

        /** @brief Reduces one Camera edit into a typed component command. */
        [[nodiscard]] EditorWorkspaceViewCommandData ApplyCameraEdit(const InspectorCameraEdit &edit, const SceneObject &object,
                                                                     bool allowCommands);

        /** @brief Reduces one Light edit into a typed component command. */
        [[nodiscard]] EditorWorkspaceViewCommandData ApplyLightEdit(const InspectorLightEdit &edit, const SceneObject &object,
                                                                    bool allowCommands);

        /** @brief Reduces one TriggerVolume edit into a typed component command. */
        [[nodiscard]] EditorWorkspaceViewCommandData ApplyTriggerVolumeEdit(const InspectorTriggerVolumeEdit &edit,
                                                                            const SceneObject &object, bool allowCommands);

        /** @brief Reduces one AudioSource edit into a typed component command. */
        [[nodiscard]] EditorWorkspaceViewCommandData ApplyAudioSourceEdit(const InspectorAudioSourceEdit &edit, const SceneObject &object,
                                                                          bool allowCommands);

        /** @brief Reports whether the current transform draft is valid. */
        [[nodiscard]] bool IsTransformValid() const noexcept;

        /** @brief Reports whether the session owns a transient transform preview. */
        [[nodiscard]] bool HasTransformPreview() const noexcept;

        /** @brief Reports whether the session owns a transient Light-component preview. */
        [[nodiscard]] bool HasLightPreview() const noexcept;

        /** @brief Reports whether the current Camera draft is valid. */
        [[nodiscard]] bool IsCameraValid() const noexcept;

        /** @brief Reports whether the current Light draft is valid. */
        [[nodiscard]] bool IsLightValid() const noexcept;

        /** @brief Reports whether the current TriggerVolume draft is valid (always true when present). */
        [[nodiscard]] bool IsTriggerVolumeValid() const noexcept;

        /** @brief Reports whether the current AudioSource draft is valid. */
        [[nodiscard]] bool IsAudioSourceValid() const noexcept;

    private:
        struct ObjectTransformBaseline {
            SceneObjectId object;
            Math::Transform localTransform;
        };

        [[nodiscard]] static SceneObjectId ResolvePrimaryId(std::span<const ObjectTransformBaseline> baselines,
                                                            std::optional<SceneObjectId> primary) noexcept;

        [[nodiscard]] static std::optional<Math::Transform> CalculateUpdatedTransform(
            const Math::Transform &baselineTransform, const InspectorObjectDraft &draft, const std::array<float, 3> &referencePosition,
            const std::array<float, 3> &referenceRotationDegrees, const std::array<float, 3> &referenceScale,
            const InspectorTransformAxisMask &editedAxes, const InspectorTransformAxisMask &relativeAxes);

        void SynchronizeDraft(std::span<const SceneObject> objects, std::span<const SceneObjectId> selectedObjects,
                              std::optional<SceneObjectId> primary, DocumentRevision revision);
        void ResetTransformDraft();
        void ResetCameraDraft(const SceneObject &object);
        void ResetLightDraft(const SceneObject &object);
        void ResetTriggerVolumeDraft(const SceneObject &object);
        void ResetAudioSourceDraft(const SceneObject &object);
        void ClearTransformInteraction() noexcept;
        [[nodiscard]] EditorWorkspaceViewCommandData RejectInvalidTransform(bool committed);
        [[nodiscard]] EditorWorkspaceViewCommandData ResetTransformToIdentity();
        [[nodiscard]] EditorWorkspaceViewCommandData CommitTransformDraft();
        [[nodiscard]] EditorWorkspaceViewCommandData PreviewTransformDraft();
        [[nodiscard]] std::optional<std::vector<SceneObjectTransformUpdate>> BuildTransformUpdates() const;

        InspectorObjectDraft m_draft;
        std::vector<ObjectTransformBaseline> m_baselines;
        InspectorTransformAxisMask m_editedAxes;
        InspectorTransformAxisMask m_relativeAxes;
        std::array<float, 3> m_referencePosition{};
        std::array<float, 3> m_referenceRotationDegrees{};
        std::array<float, 3> m_referenceScale{1.0F, 1.0F, 1.0F};
        bool m_hasTransformPreview{false};
        bool m_hasLightPreview{false};
    };
}  // namespace Horo::Editor
