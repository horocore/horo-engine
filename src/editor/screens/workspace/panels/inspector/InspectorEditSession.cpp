#include "editor/screens/workspace/panels/inspector/InspectorEditSession.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

namespace Horo::Editor {
    namespace {
        constexpr float RadiansToDegrees = 180.0F / std::numbers::pi_v<float>;
        constexpr float DegreesToRadians = std::numbers::pi_v<float> / 180.0F;
        constexpr float MixedValueTolerance = 0.0001F;

        [[nodiscard]] std::array<float, 3> ToArray(const Math::Vec3 value) noexcept {
            return {value.x, value.y, value.z};
        }

        [[nodiscard]] Math::Vec3 ToVec3(const std::array<float, 3> &value) noexcept {
            return {value[0], value[1], value[2]};
        }

        [[nodiscard]] std::array<float, 3> ToEulerDegrees(const Math::Quaternion &rotation) noexcept {
            const Math::Vec3 radians = rotation.ToEulerRadians();
            return {radians.x * RadiansToDegrees, radians.y * RadiansToDegrees, radians.z * RadiansToDegrees};
        }

        [[nodiscard]] bool IsFinite(const std::array<float, 3> &value) noexcept {
            return std::ranges::all_of(value, [](const float component) {
                return std::isfinite(component);
            });
        }

        [[nodiscard]] bool Differs(const float left, const float right) noexcept {
            return std::fabs(left - right) > MixedValueTolerance;
        }

        void MergeAxisMask(std::array<bool, 3> &destination, const std::array<bool, 3> &source) noexcept {
            for (std::size_t axis = 0; axis < destination.size(); ++axis)
                destination[axis] = destination[axis] || source[axis];
        }

        void ClearMixedAxes(std::array<bool, 3> &mixed, const std::array<bool, 3> &edited) noexcept {
            for (std::size_t axis = 0; axis < mixed.size(); ++axis) {
                if (edited[axis])
                    mixed[axis] = false;
            }
        }

        void CaptureRelativeAxes(std::array<bool, 3> &relative, const std::array<bool, 3> &mixed,
                                 const std::array<bool, 3> &previouslyEdited, const std::array<bool, 3> &changed) noexcept {
            for (std::size_t axis = 0; axis < relative.size(); ++axis) {
                if (changed[axis] && !previouslyEdited[axis])
                    relative[axis] = mixed[axis];
            }
        }

        [[nodiscard]] EditorWorkspaceViewCommandData MakeObjectCommand(const EditorWorkspaceViewCommand command,
                                                                       const SceneObjectId object) {
            EditorWorkspaceViewCommandData result;
            result.command = command;
            result.objectPayload = object;
            return result;
        }
    }  // namespace

    SceneObjectId InspectorEditSession::ResolvePrimaryId(const std::span<const ObjectTransformBaseline> baselines,
                                                         const std::optional<SceneObjectId> primary) noexcept {
        if (primary.has_value() && std::ranges::find(baselines, *primary, &ObjectTransformBaseline::object) != baselines.end())
            return *primary;
        return baselines.empty() ? SceneObjectId{} : baselines.front().object;
    }

    std::optional<Math::Transform> InspectorEditSession::CalculateUpdatedTransform(
        const Math::Transform &baselineTransform, const InspectorObjectDraft &draft, const std::array<float, 3> &referencePosition,
        const std::array<float, 3> &referenceRotationDegrees, const std::array<float, 3> &referenceScale,
        const InspectorTransformAxisMask &editedAxes, const InspectorTransformAxisMask &relativeAxes) {
        std::array position = ToArray(baselineTransform.translation);
        const std::array baselineRotation = ToEulerDegrees(baselineTransform.rotation);
        std::array rotation = baselineRotation;
        std::array scale = ToArray(baselineTransform.scale);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (editedAxes.position[axis])
                position[axis] =
                    relativeAxes.position[axis] ? position[axis] + (draft.position[axis] - referencePosition[axis]) : draft.position[axis];
            if (editedAxes.rotation[axis])
                rotation[axis] = relativeAxes.rotation[axis]
                                     ? rotation[axis] + (draft.rotationDegrees[axis] - referenceRotationDegrees[axis])
                                     : draft.rotationDegrees[axis];
            if (editedAxes.scale[axis])
                scale[axis] = relativeAxes.scale[axis] ? scale[axis] + (draft.scale[axis] - referenceScale[axis]) : draft.scale[axis];
        }
        Math::Quaternion updatedRotation = baselineTransform.rotation;
        bool rotationChanged = false;
        for (std::size_t axis = 0; axis < editedAxes.rotation.size(); ++axis) {
            rotationChanged = rotationChanged || (editedAxes.rotation[axis] && Differs(rotation[axis], baselineRotation[axis]));
        }
        if (rotationChanged) {
            const Math::Vec3 rotationRadians{rotation[0] * DegreesToRadians, rotation[1] * DegreesToRadians,
                                             rotation[2] * DegreesToRadians};
            const Result<Math::Quaternion> quaternion = Math::Quaternion::TryFromEulerRadians(rotationRadians);
            if (quaternion.HasError())
                return std::nullopt;
            updatedRotation = quaternion.Value();
        }
        return Math::Transform{
            .translation = ToVec3(position),
            .rotation = updatedRotation,
            .scale = ToVec3(scale),
        };
    }

    bool InspectorTransformAxisMask::Any() const noexcept {
        return std::ranges::any_of(position, std::identity{}) || std::ranges::any_of(rotation, std::identity{}) ||
               std::ranges::any_of(scale, std::identity{});
    }

    /** @copydoc InspectorEditSession::Draft */
    InspectorObjectDraft &InspectorEditSession::Draft() noexcept {
        return m_draft;
    }

    /** @copydoc InspectorEditSession::Draft */
    const InspectorObjectDraft &InspectorEditSession::Draft() const noexcept {
        return m_draft;
    }

    /** @copydoc InspectorEditSession::BeginObject */
    EditorWorkspaceViewCommandData InspectorEditSession::BeginObject(const SceneObject &object, const DocumentRevision revision) {
        const std::array selected{object.id};
        const std::array objects{object};
        return BeginSelection(objects, selected, object.id, revision);
    }

    /** @copydoc InspectorEditSession::BeginSelection */
    EditorWorkspaceViewCommandData InspectorEditSession::BeginSelection(const std::span<const SceneObject> objects,
                                                                        const std::span<const SceneObjectId> selectedObjects,
                                                                        const std::optional<SceneObjectId> primary,
                                                                        const DocumentRevision revision) {
        const bool sameSelection =
            m_baselines.size() == selectedObjects.size() &&
            std::ranges::equal(m_baselines, selectedObjects, std::ranges::equal_to{}, &ObjectTransformBaseline::object, std::identity{});
        const SceneObjectId resolvedPrimary = ResolvePrimaryId(m_baselines, primary);
        const bool samePrimary = sameSelection && m_draft.object == resolvedPrimary;
        EditorWorkspaceViewCommandData command;
        if (!sameSelection || !samePrimary || m_draft.revision != revision) {
            if (m_hasTransformPreview)
                command.command = EditorWorkspaceViewCommand::CancelObjectTransformPreview;
            else if (m_hasLightPreview)
                command.command = EditorWorkspaceViewCommand::CancelLightComponentPreview;
            m_hasTransformPreview = false;
            m_hasLightPreview = false;
        }
        SynchronizeDraft(objects, selectedObjects, primary, revision);
        return command;
    }

    /** @copydoc InspectorEditSession::Clear */
    EditorWorkspaceViewCommandData InspectorEditSession::Clear() {
        EditorWorkspaceViewCommandData command;
        if (m_hasTransformPreview) {
            command.command = EditorWorkspaceViewCommand::CancelObjectTransformPreview;
        } else if (m_hasLightPreview) {
            command.command = EditorWorkspaceViewCommand::CancelLightComponentPreview;
        }
        m_hasTransformPreview = false;
        m_hasLightPreview = false;
        m_baselines.clear();
        m_editedAxes = {};
        m_relativeAxes = {};
        m_draft = {};
        return command;
    }

    /** @copydoc InspectorEditSession::ApplyNameEdit */
    EditorWorkspaceViewCommandData InspectorEditSession::ApplyNameEdit(const InspectorNameEdit &edit, const SceneObject &object,
                                                                       const bool allowCommands) {
        if (m_baselines.size() != 1 || m_draft.object != object.id)
            return {};
        if (edit.cancelled) {
            m_draft.name = object.name;
            return {};
        }
        if (!allowCommands || !edit.committed || !IsValidSceneObjectName(m_draft.name) || m_draft.name == object.name)
            return {};

        EditorWorkspaceViewCommandData command = MakeObjectCommand(EditorWorkspaceViewCommand::UpdateObjectName, object.id);
        command.stringPayload = m_draft.name;
        return command;
    }

    /** @copydoc InspectorEditSession::ApplyTransformEdit */
    EditorWorkspaceViewCommandData InspectorEditSession::ApplyTransformEdit(const InspectorTransformEdit &edit, const bool allowCommands) {
        if (!allowCommands || m_baselines.empty())
            return {};

        if (edit.resetRequested)
            return ResetTransformToIdentity();

        if (edit.cancelRequested && m_hasTransformPreview) {
            ResetTransformDraft();
            ClearTransformInteraction();
            return MakeObjectCommand(EditorWorkspaceViewCommand::CancelObjectTransformPreview, m_baselines.front().object);
        }

        if (edit.changed) {
            CaptureRelativeAxes(m_relativeAxes.position, m_draft.mixed.position, m_editedAxes.position, edit.changedAxes.position);
            CaptureRelativeAxes(m_relativeAxes.rotation, m_draft.mixed.rotation, m_editedAxes.rotation, edit.changedAxes.rotation);
            CaptureRelativeAxes(m_relativeAxes.scale, m_draft.mixed.scale, m_editedAxes.scale, edit.changedAxes.scale);
            MergeAxisMask(m_editedAxes.position, edit.changedAxes.position);
            MergeAxisMask(m_editedAxes.rotation, edit.changedAxes.rotation);
            MergeAxisMask(m_editedAxes.scale, edit.changedAxes.scale);
            ClearMixedAxes(m_draft.mixed.position, edit.changedAxes.position);
            ClearMixedAxes(m_draft.mixed.rotation, edit.changedAxes.rotation);
            ClearMixedAxes(m_draft.mixed.scale, edit.changedAxes.scale);
        }

        if (!IsTransformValid()) {
            return RejectInvalidTransform(edit.committed);
        }

        if (edit.committed)
            return CommitTransformDraft();
        if (!edit.changed || !m_editedAxes.Any())
            return {};

        return PreviewTransformDraft();
    }

    EditorWorkspaceViewCommandData InspectorEditSession::ResetTransformToIdentity() {
        m_draft.position = {};
        m_draft.rotationDegrees = {};
        m_draft.scale = {1.0F, 1.0F, 1.0F};
        m_draft.mixed = {};
        m_editedAxes = {
            .position = {true, true, true},
            .rotation = {true, true, true},
            .scale = {true, true, true},
        };
        m_relativeAxes = {};
        return CommitTransformDraft();
    }

    EditorWorkspaceViewCommandData InspectorEditSession::RejectInvalidTransform(const bool committed) {
        if (!committed || !m_hasTransformPreview)
            return {};
        ResetTransformDraft();
        ClearTransformInteraction();
        return MakeObjectCommand(EditorWorkspaceViewCommand::CancelObjectTransformPreview, m_baselines.front().object);
    }

    EditorWorkspaceViewCommandData InspectorEditSession::CommitTransformDraft() {
        if (!m_editedAxes.Any()) {
            ClearTransformInteraction();
            return {};
        }

        const bool hadPreview = m_hasTransformPreview;
        const std::optional<std::vector<SceneObjectTransformUpdate>> updates = BuildTransformUpdates();
        ClearTransformInteraction();
        if (!updates.has_value() || updates->empty()) {
            ResetTransformDraft();
            return hadPreview ? MakeObjectCommand(EditorWorkspaceViewCommand::CancelObjectTransformPreview, m_baselines.front().object)
                              : EditorWorkspaceViewCommandData{};
        }

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::CommitObjectTransform;
        command.transformUpdates = std::move(*updates);
        return command;
    }

    EditorWorkspaceViewCommandData InspectorEditSession::PreviewTransformDraft() {
        const std::optional<std::vector<SceneObjectTransformUpdate>> updates = BuildTransformUpdates();
        if (!updates.has_value() || updates->empty()) {
            const bool hadPreview = m_hasTransformPreview;
            ResetTransformDraft();
            ClearTransformInteraction();
            return hadPreview ? MakeObjectCommand(EditorWorkspaceViewCommand::CancelObjectTransformPreview, m_baselines.front().object)
                              : EditorWorkspaceViewCommandData{};
        }
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        command.transformUpdates = std::move(*updates);
        m_hasTransformPreview = true;
        return command;
    }

    /** @copydoc InspectorEditSession::ApplyCameraEdit */
    EditorWorkspaceViewCommandData InspectorEditSession::ApplyCameraEdit(const InspectorCameraEdit &edit, const SceneObject &object,
                                                                         const bool allowCommands) {
        if (m_baselines.size() != 1 || m_draft.object != object.id || !object.components.camera.has_value() ||
            !m_draft.camera.has_value()) {
            return {};
        }
        if (edit.cancelRequested) {
            ResetCameraDraft(object);
            return {};
        }
        if (!allowCommands || !edit.committed || !IsValidCameraComponent(*m_draft.camera) || *m_draft.camera == *object.components.camera)
            return {};

        EditorWorkspaceViewCommandData command = MakeObjectCommand(EditorWorkspaceViewCommand::UpdateCameraComponent, object.id);
        command.cameraPayload = *m_draft.camera;
        return command;
    }

    /** @copydoc InspectorEditSession::ApplyLightEdit */
    EditorWorkspaceViewCommandData InspectorEditSession::ApplyLightEdit(const InspectorLightEdit &edit, const SceneObject &object,
                                                                        const bool allowCommands) {
        if (!allowCommands || m_baselines.size() != 1 || m_draft.object != object.id || !object.components.light.has_value() ||
            !m_draft.light.has_value())
            return {};

        if (edit.cancelRequested && m_hasLightPreview) {
            ResetLightDraft(object);
            m_hasLightPreview = false;
            return MakeObjectCommand(EditorWorkspaceViewCommand::CancelLightComponentPreview, object.id);
        }

        if (!IsValidLightComponent(*m_draft.light)) {
            if (edit.committed && m_hasLightPreview) {
                ResetLightDraft(object);
                m_hasLightPreview = false;
                return MakeObjectCommand(EditorWorkspaceViewCommand::CancelLightComponentPreview, object.id);
            }
            return {};
        }

        if (edit.committed) {
            const bool hadPreview = m_hasLightPreview;
            m_hasLightPreview = false;
            if (*m_draft.light == *object.components.light)
                return hadPreview ? MakeObjectCommand(EditorWorkspaceViewCommand::CancelLightComponentPreview, object.id)
                                  : EditorWorkspaceViewCommandData{};
            EditorWorkspaceViewCommandData command = MakeObjectCommand(EditorWorkspaceViewCommand::UpdateLightComponent, object.id);
            command.lightPayload = *m_draft.light;
            return command;
        }

        if (!edit.changed)
            return {};
        if (*m_draft.light == *object.components.light) {
            if (!m_hasLightPreview)
                return {};
            m_hasLightPreview = false;
            return MakeObjectCommand(EditorWorkspaceViewCommand::CancelLightComponentPreview, object.id);
        }

        EditorWorkspaceViewCommandData command = MakeObjectCommand(EditorWorkspaceViewCommand::PreviewLightComponent, object.id);
        command.lightPayload = *m_draft.light;
        m_hasLightPreview = true;
        return command;
    }

    /** @copydoc InspectorEditSession::IsTransformValid */
    bool InspectorEditSession::IsTransformValid() const noexcept {
        return IsFinite(m_draft.position) && IsFinite(m_draft.rotationDegrees) && IsFinite(m_draft.scale);
    }

    /** @copydoc InspectorEditSession::HasTransformPreview */
    bool InspectorEditSession::HasTransformPreview() const noexcept {
        return m_hasTransformPreview;
    }

    /** @copydoc InspectorEditSession::HasLightPreview */
    bool InspectorEditSession::HasLightPreview() const noexcept {
        return m_hasLightPreview;
    }

    /** @copydoc InspectorEditSession::IsCameraValid */
    bool InspectorEditSession::IsCameraValid() const noexcept {
        return m_draft.camera.has_value() && IsValidCameraComponent(*m_draft.camera);
    }

    /** @copydoc InspectorEditSession::IsLightValid */
    bool InspectorEditSession::IsLightValid() const noexcept {
        return m_draft.light.has_value() && IsValidLightComponent(*m_draft.light);
    }

    /** @copydoc InspectorEditSession::IsTriggerVolumeValid */
    bool InspectorEditSession::IsTriggerVolumeValid() const noexcept {
        return m_draft.triggerVolume.has_value();
    }

    /** @copydoc InspectorEditSession::IsAudioSourceValid */
    bool InspectorEditSession::IsAudioSourceValid() const noexcept {
        return m_draft.audioSource.has_value() && IsValidAudioSourceComponent(*m_draft.audioSource);
    }

    /** @copydoc InspectorEditSession::ApplyTriggerVolumeEdit */
    EditorWorkspaceViewCommandData InspectorEditSession::ApplyTriggerVolumeEdit(const InspectorTriggerVolumeEdit &edit,
                                                                                const SceneObject &object, const bool allowCommands) {
        if (m_baselines.size() != 1 || m_draft.object != object.id || !object.components.triggerVolume.has_value() ||
            !m_draft.triggerVolume.has_value()) {
            return {};
        }
        if (edit.cancelRequested) {
            ResetTriggerVolumeDraft(object);
            return {};
        }
        if (!allowCommands || !edit.committed || *m_draft.triggerVolume == *object.components.triggerVolume)
            return {};

        EditorWorkspaceViewCommandData command = MakeObjectCommand(EditorWorkspaceViewCommand::UpdateTriggerVolumeComponent, object.id);
        command.triggerVolumePayload = *m_draft.triggerVolume;
        return command;
    }

    /** @copydoc InspectorEditSession::ApplyAudioSourceEdit */
    EditorWorkspaceViewCommandData InspectorEditSession::ApplyAudioSourceEdit(const InspectorAudioSourceEdit &edit,
                                                                              const SceneObject &object, const bool allowCommands) {
        if (m_baselines.size() != 1 || m_draft.object != object.id || !object.components.audioSource.has_value() ||
            !m_draft.audioSource.has_value()) {
            return {};
        }
        if (edit.cancelRequested) {
            ResetAudioSourceDraft(object);
            return {};
        }
        if (!allowCommands || !edit.committed || !IsValidAudioSourceComponent(*m_draft.audioSource) ||
            *m_draft.audioSource == *object.components.audioSource)
            return {};

        EditorWorkspaceViewCommandData command = MakeObjectCommand(EditorWorkspaceViewCommand::UpdateAudioSourceComponent, object.id);
        command.audioSourcePayload = *m_draft.audioSource;
        return command;
    }

    void InspectorEditSession::ResetCameraDraft(const SceneObject &object) {
        m_draft.camera = object.components.camera;
        if (m_draft.camera.has_value())
            m_draft.cameraFieldOfViewDegrees = m_draft.camera->verticalFieldOfViewRadians * RadiansToDegrees;
    }

    void InspectorEditSession::ResetLightDraft(const SceneObject &object) {
        m_draft.light = object.components.light;
        if (!m_draft.light.has_value())
            return;
        m_draft.lightInnerConeDegrees = m_draft.light->innerConeRadians * RadiansToDegrees;
        m_draft.lightOuterConeDegrees = m_draft.light->outerConeRadians * RadiansToDegrees;
    }

    void InspectorEditSession::ResetTriggerVolumeDraft(const SceneObject &object) {
        m_draft.triggerVolume = object.components.triggerVolume;
    }

    void InspectorEditSession::ResetAudioSourceDraft(const SceneObject &object) {
        m_draft.audioSource = object.components.audioSource;
    }

    void InspectorEditSession::ClearTransformInteraction() noexcept {
        m_hasTransformPreview = false;
        m_editedAxes = {};
        m_relativeAxes = {};
    }

    void InspectorEditSession::SynchronizeDraft(const std::span<const SceneObject> objects,
                                                const std::span<const SceneObjectId> selectedObjects,
                                                const std::optional<SceneObjectId> primary, const DocumentRevision revision) {
        const bool sameSelection =
            m_baselines.size() == selectedObjects.size() &&
            std::ranges::equal(m_baselines, selectedObjects, std::ranges::equal_to{}, &ObjectTransformBaseline::object, std::identity{});
        if (const SceneObjectId resolvedPrimary = ResolvePrimaryId(m_baselines, primary);
            sameSelection && m_draft.object == resolvedPrimary && m_draft.revision == revision) {
            return;
        }

        m_baselines.clear();
        m_baselines.reserve(selectedObjects.size());
        for (const SceneObjectId selected : selectedObjects) {
            const auto object = std::ranges::find(objects, selected, &SceneObject::id);
            if (object != objects.end())
                m_baselines.emplace_back(object->id, object->localTransform);
        }

        m_editedAxes = {};
        m_relativeAxes = {};
        m_draft = {};
        m_draft.revision = revision;
        m_draft.selectedObjectCount = m_baselines.size();
        if (m_baselines.empty())
            return;

        const SceneObjectId primaryId = ResolvePrimaryId(m_baselines, primary);
        const auto primaryObject = std::ranges::find(objects, primaryId, &SceneObject::id);
        if (primaryObject == objects.end())
            return;

        m_draft.object = primaryId;
        m_draft.name = primaryObject->name;
        m_draft.camera = m_baselines.size() == 1 ? primaryObject->components.camera : std::nullopt;
        if (m_draft.camera.has_value())
            m_draft.cameraFieldOfViewDegrees = m_draft.camera->verticalFieldOfViewRadians * RadiansToDegrees;
        m_draft.light = m_baselines.size() == 1 ? primaryObject->components.light : std::nullopt;
        if (m_draft.light.has_value()) {
            m_draft.lightInnerConeDegrees = m_draft.light->innerConeRadians * RadiansToDegrees;
            m_draft.lightOuterConeDegrees = m_draft.light->outerConeRadians * RadiansToDegrees;
        }
        m_draft.triggerVolume = m_baselines.size() == 1 ? primaryObject->components.triggerVolume : std::nullopt;
        m_draft.audioSource = m_baselines.size() == 1 ? primaryObject->components.audioSource : std::nullopt;
        ResetTransformDraft();
    }

    void InspectorEditSession::ResetTransformDraft() {
        if (m_baselines.empty())
            return;

        const SceneObjectId targetId = m_draft.object.has_value() ? *m_draft.object : m_baselines.front().object;
        const auto primary = std::ranges::find(m_baselines, targetId, &ObjectTransformBaseline::object);
        const Math::Transform &primaryTransform =
            primary != m_baselines.end() ? primary->localTransform : m_baselines.front().localTransform;
        m_draft.position = ToArray(primaryTransform.translation);
        m_draft.rotationDegrees = ToEulerDegrees(primaryTransform.rotation);
        m_draft.scale = ToArray(primaryTransform.scale);
        m_referencePosition = m_draft.position;
        m_referenceRotationDegrees = m_draft.rotationDegrees;
        m_referenceScale = m_draft.scale;
        m_draft.mixed = {};

        for (const ObjectTransformBaseline &baseline : m_baselines) {
            const std::array position = ToArray(baseline.localTransform.translation);
            const std::array rotation = ToEulerDegrees(baseline.localTransform.rotation);
            const std::array scale = ToArray(baseline.localTransform.scale);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                m_draft.mixed.position[axis] = m_draft.mixed.position[axis] || Differs(position[axis], m_draft.position[axis]);
                m_draft.mixed.rotation[axis] = m_draft.mixed.rotation[axis] || Differs(rotation[axis], m_draft.rotationDegrees[axis]);
                m_draft.mixed.scale[axis] = m_draft.mixed.scale[axis] || Differs(scale[axis], m_draft.scale[axis]);
            }
        }
        m_editedAxes = {};
        m_relativeAxes = {};
    }

    std::optional<std::vector<SceneObjectTransformUpdate>> InspectorEditSession::BuildTransformUpdates() const {
        std::vector<SceneObjectTransformUpdate> updates;
        updates.reserve(m_baselines.size());
        for (const ObjectTransformBaseline &baseline : m_baselines) {
            const auto updated = CalculateUpdatedTransform(baseline.localTransform, m_draft, m_referencePosition,
                                                           m_referenceRotationDegrees, m_referenceScale, m_editedAxes, m_relativeAxes);
            if (!updated.has_value())
                return std::nullopt;
            if (*updated != baseline.localTransform)
                updates.emplace_back(baseline.object, *updated);
        }
        return updates;
    }
}  // namespace Horo::Editor
