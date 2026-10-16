#include "InspectorEditSession.h"

#include <charconv>
#include <string_view>

namespace Horo::Editor {
    namespace {
        /** @brief Parses the complete decimal stable identity without float narrowing or partial acceptance. */
        [[nodiscard]] bool PositiveInteger(std::string_view text, std::uint64_t &value) noexcept {
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value != 0;
        }

        /** @brief Resolves an optional authored UUID without selecting a project authority. */
        template <typename T> [[nodiscard]] bool Identity(const std::string &text, std::optional<T> &target) {
            if (text.empty()) {
                target.reset();
                return true;
            }
            auto parsed = T::Parse(text);
            if (parsed.HasError())
                return false;
            target = std::move(parsed).Value();
            return true;
        }

        /** @brief Admits material and excluded-body pairs atomically; incomplete pairs remain presentation-only errors. */
        [[nodiscard]] bool BindingPairs(const std::array<std::string, 9> &text, Runtime::CharacterSceneCollision &collision) {
            if (!text[2].empty() || !text[3].empty()) {
                auto material = Assets::AssetId::Parse(text[2]);
                std::uint64_t slot{};
                if (material.HasError() || !PositiveInteger(text[3], slot))
                    return false;
                collision.defaultMaterial =
                    Runtime::PhysicsColliderMaterialBinding{Physics::PhysicsMaterialSlotId::FromValue(slot), material.Value()};
            } else
                collision.defaultMaterial.reset();
            if (!text[6].empty() || !text[7].empty()) {
                std::uint64_t object{}, body{};
                if (!PositiveInteger(text[6], object) || !PositiveInteger(text[7], body))
                    return false;
                collision.excludedBody = Runtime::PhysicsBodyReference{{object}, {body}};
            } else
                collision.excludedBody.reset();
            return true;
        }

        /** @brief Commits all valid binding text into a copied fixed typed value, never a partially parsed component. */
        [[nodiscard]] bool ResolveBindings(InspectorObjectDraft &draft) {
            auto collision = draft.characterController->collision;
            const auto &text = draft.characterBindings.text;
            std::uint64_t contacts{};
            if (!Identity(text[0], collision.profile) || !Identity(text[1], collision.queryChannel) ||
                !Identity(text[4], collision.requiredLayer) || !Identity(text[5], collision.requiredProfile) ||
                !BindingPairs(text, collision) || !PositiveInteger(text[8], contacts) || contacts > Character::MaximumCharacterContacts)
                return false;
            draft.characterController->collision = collision;
            draft.characterController->movement.maximumContacts = static_cast<std::uint32_t>(contacts);
            return true;
        }

        /** @brief Captures editable strings only on selection/revision/cancel transitions. */
        [[nodiscard]] InspectorCharacterBindings CaptureBindings(const Runtime::CharacterControllerComponent &component) {
            const auto &collision = component.collision;
            return {{{collision.profile ? collision.profile->ToString() : "",
                      collision.queryChannel ? collision.queryChannel->ToString() : "",
                      collision.defaultMaterial ? collision.defaultMaterial->material.ToString() : "",
                      collision.defaultMaterial ? std::to_string(collision.defaultMaterial->slot.Value()) : "",
                      collision.requiredLayer ? collision.requiredLayer->ToString() : "",
                      collision.requiredProfile ? collision.requiredProfile->ToString() : "",
                      collision.excludedBody ? std::to_string(collision.excludedBody->object.value) : "",
                      collision.excludedBody ? std::to_string(collision.excludedBody->body.value) : "",
                      std::to_string(component.movement.maximumContacts)}},
                    true,
                    Runtime::ValidateCharacterControllerComponent(component).HasValue()};
        }
    }  // namespace

    /** @copydoc InspectorEditSession::ResetCharacterDraft */
    void InspectorEditSession::ResetCharacterDraft(const SceneObject &object) {
        m_draft.characterController = object.components.characterController;
        m_draft.characterBindings =
            m_draft.characterController ? CaptureBindings(*m_draft.characterController) : InspectorCharacterBindings{};
    }

    /** @copydoc InspectorEditSession::IsCharacterValid */
    bool InspectorEditSession::IsCharacterValid() const {
        return m_draft.characterController && m_draft.characterBindings.valid && m_draft.characterBindings.componentValid;
    }

    /** @copydoc InspectorEditSession::ApplyCharacterEdit */
    EditorWorkspaceViewCommandData InspectorEditSession::ApplyCharacterEdit(const InspectorCharacterEdit &edit, const SceneObject &object,
                                                                            const bool allowCommands) {
        if (m_baselines.size() != 1 || m_draft.object != object.id || !object.components.characterController ||
            !m_draft.characterController)
            return {};
        if (edit.cancelRequested) {
            ResetCharacterDraft(object);
            return {};
        }
        if (edit.changed || edit.committed) {
            m_draft.characterBindings.valid = ResolveBindings(m_draft);
            m_draft.characterBindings.componentValid =
                m_draft.characterBindings.valid && Runtime::ValidateCharacterControllerComponent(*m_draft.characterController).HasValue();
        }
        if (!allowCommands || !edit.committed || !IsCharacterValid() ||
            *m_draft.characterController == *object.components.characterController)
            return {};
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::UpdateCharacterComponent;
        command.objectPayload = object.id;
        command.characterPayload = m_draft.characterController;
        return command;
    }
}  // namespace Horo::Editor
