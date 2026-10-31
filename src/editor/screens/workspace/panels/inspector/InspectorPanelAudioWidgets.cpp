#include "Horo/Editor/GuiLiveReload.h"
#include "component_fields/definitions/AudioListenerFields.h"
#include "component_fields/definitions/AudioSourceFields.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanelInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <variant>

namespace Horo::Editor {
    using namespace InspectorPanelDetail;

    namespace {
        /** @brief Resolves localized audio-inspector copy without changing authored identity. */
        const char *Label(const EditorGuiContext &context, const char *key) {
            return context.localization.Get("editor", key).c_str();
        }

        /** @brief Edits a stable typed identity exactly; zero remains an explicit invalid draft. */
        template <typename Id> bool DrawIdentity(const EditorGuiContext &context, const char *key, Id &id) {
            std::uint64_t value = id.Value();
            const auto edit = Ui::DrawUnsignedPropRow(Label(context, key), key, value, context.theme.fonts, value == 0);
            if (edit.changed) {
                auto parsed = GuiLive::HostSymbol<&Id::Create>()(value);
                id = parsed.HasValue() ? parsed.Value() : Id{};
            }
            return edit.committed;
        }

        /** @brief Parses persistent asset UUIDs into their distinct Audio-owned asset type. */
        template <typename Id> bool DrawAssetIdentity(const EditorGuiContext &context, const char *key, Id &id, std::string &text) {
            Ui::DrawPropRow(Label(context, key), "", context.theme.fonts);
            const bool changed = Ui::InputTextControl(key, text, 36, context.theme.fonts);
            const bool committed = ImGui::IsItemDeactivatedAfterEdit();
            if (changed) {
                const auto parsed = Assets::AssetId::Parse(text);
                id = parsed.HasValue() ? GuiLive::HostSymbol<&Id::Create>()(parsed.Value()).Value() : Id{};
            }
            return committed;
        }

        /** @brief Edits an optional identity, with zero meaning explicit default routing/no group. */
        template <typename Id> bool DrawOptionalIdentity(const EditorGuiContext &context, const char *key, std::optional<Id> &id) {
            std::uint64_t value = id ? id->Value() : 0;
            const auto edit = Ui::DrawUnsignedPropRow(Label(context, key), key, value, context.theme.fonts);
            if (edit.changed)
                id = value == 0 ? std::nullopt : std::optional{GuiLive::HostSymbol<&Id::Create>()(value).Value()};
            return edit.committed;
        }

        /** @brief Edits the complete typed reference family and the identities owned by that family. */
        bool DrawSoundReference(const EditorGuiContext &context, InspectorObjectDraft &draft) {
            auto &sound = draft.audioSource->sound;
            const std::array entries{Label(context, "workspace.inspector.audio.unassigned"),
                                     Label(context, "workspace.inspector.audio.clip"),
                                     Label(context, "workspace.inspector.audio.variation"),
                                     Label(context, "workspace.inspector.audio.stream"),
                                     Label(context, "workspace.inspector.audio.music"),
                                     Label(context, "workspace.inspector.audio.extension")};
            auto kind = static_cast<int>(sound.kind);
            bool committed = Ui::DrawComboPropRow(Label(context, "workspace.inspector.audio.sound_kind"), "audio_kind", kind, entries,
                                                  context.theme.fonts);
            if (committed) {
                draft.audioSoundIdentity.clear();
                sound.kind = static_cast<Audio::AudioSoundReferenceKind>(kind);
                using enum Audio::AudioSoundReferenceKind;
                switch (sound.kind) {
                    case Unassigned:
                        sound.target = std::monostate{};
                        break;
                    case Clip:
                        sound.target = Audio::AudioClipId{};
                        break;
                    case Extension:
                        sound.target = Audio::AudioSoundExtensionReference{};
                        break;
                    default:
                        sound.target = Audio::AudioSoundId{};
                        break;
                }
            }
            if (auto *clip = std::get_if<Audio::AudioClipId>(&sound.target))
                committed = DrawAssetIdentity(context, "workspace.inspector.audio.sound_id", *clip, draft.audioSoundIdentity) || committed;
            if (auto *definition = std::get_if<Audio::AudioSoundId>(&sound.target))
                committed =
                    DrawAssetIdentity(context, "workspace.inspector.audio.sound_id", *definition, draft.audioSoundIdentity) || committed;
            if (auto *extension = std::get_if<Audio::AudioSoundExtensionReference>(&sound.target)) {
                committed = DrawIdentity(context, "workspace.inspector.audio.provider_id", extension->contribution) || committed;
                committed =
                    DrawAssetIdentity(context, "workspace.inspector.audio.sound_id", extension->definition, draft.audioSoundIdentity) ||
                    committed;
            }
            return committed;
        }

        /** @brief Keeps typed asset/routing identity editing with its owning transaction adapters. */
        bool DrawPlaybackReferences(const EditorGuiContext &context, InspectorObjectDraft &draft) {
            const bool sound = DrawSoundReference(context, draft);
            const bool bus = DrawOptionalIdentity(context, "workspace.inspector.audio.bus", draft.audioSource->playback.bus);
            const bool group =
                DrawOptionalIdentity(context, "workspace.inspector.audio.concurrency_group", draft.audioSource->playback.concurrency.group);
            return sound || bus || group;
        }

        /** @brief Maps a typed preview failure to localized copy without changing its retained diagnostic. */
        const char *PreviewFailureMessage(const Error &error) {
            const auto &code = error.code.Value();
            if (code == GuiLive::HostSymbol<&AudioComponentPreviewErrors::MissingAsset>().code.Value())
                return "workspace.inspector.audio.asset_missing";
            if (code == GuiLive::HostSymbol<&AudioComponentPreviewErrors::MissingListener>().code.Value())
                return "workspace.inspector.audio.listener_missing";
            if (code == GuiLive::HostSymbol<&AudioComponentPreviewErrors::MissingBus>().code.Value())
                return "workspace.inspector.audio.bus_missing";
            if (code == GuiLive::HostSymbol<&AudioComponentPreviewErrors::ProviderUnavailable>().code.Value())
                return "workspace.inspector.audio.provider_missing";
            if (code == GuiLive::HostSymbol<&AudioComponentPreviewErrors::Busy>().code.Value())
                return "workspace.inspector.audio.busy";
            if (code == GuiLive::HostSymbol<&AudioComponentPreviewErrors::Stale>().code.Value())
                return "workspace.inspector.audio.stale";
            return "workspace.inspector.audio.preview_failed";
        }

        /** @brief Projects native-independent frontend progress into the existing localized status surface. */
        const char *PreviewPhaseMessage(const Audio::AudioFrontendPhase phase) {
            using enum Audio::AudioFrontendPhase;
            switch (phase) {
                case Active:
                    return "workspace.inspector.audio.preview_active";
                case Closed:
                    return "workspace.inspector.audio.preview_closed";
                case Closing:
                case Retained:
                    return "workspace.inspector.audio.preview_closing";
                default:
                    return "workspace.inspector.audio.preview_opening";
            }
        }
    }  // namespace

    InspectorAudioSourceEdit InspectorPanel::DrawAudioSourceWidgets(const EditorGuiContext &context) {
        InspectorObjectDraft &draft = m_editSession.Draft();
        if (!draft.audioSource.has_value())
            return {};

        Ui::Card card(Ui::CardProps{.id = "##AudioSourceCard"});
        const ComponentTitleBarResult header =
            DrawComponentTitleBar(card, InspectorPanel::kTextScope.Text(context.localization, "inspector.audio_source").c_str(),
                                  {.enabled = draft.audioSource->enabled}, context);
        if (header.resetRequested) {
            const bool enabled = draft.audioSource->enabled;
            draft.audioSource = Runtime::AudioSourceComponent{};
            draft.audioSource->enabled = enabled;
        }
        if (header.toggleEnabledRequested)
            draft.audioSource->enabled = !draft.audioSource->enabled;
        const bool cancelRequested = IsInspectorCardEscapeRequested();
        if (!card.BeginBody())
            return {.cancelRequested = cancelRequested,
                    .committed = header.resetRequested || header.toggleEnabledRequested,
                    .removeRequested = header.removeRequested};
        ImGui::BeginDisabled(!draft.audioSource->enabled);

        const auto fields = InspectorFields::Draw(*draft.audioSource, context);
        const bool references = DrawPlaybackReferences(context, draft);
        const bool committed = references || fields.committed || header.resetRequested || header.toggleEnabledRequested;
        ImGui::EndDisabled();
        card.Finish();
        return {.cancelRequested = cancelRequested, .committed = committed, .removeRequested = header.removeRequested};
    }

    InspectorAudioListenerEdit InspectorPanel::DrawAudioListenerWidgets(const EditorGuiContext &context) {
        auto &listener = m_editSession.Draft().audioListener;
        if (!listener)
            return {};
        Ui::Card card(Ui::CardProps{.id = "##AudioListenerCard"});
        const auto header =
            DrawComponentTitleBar(card, Label(context, "workspace.inspector.audio_listener"), {.enabled = listener->enabled}, context);
        if (header.resetRequested)
            listener = Runtime::AudioListenerComponent{.enabled = listener->enabled};
        if (header.toggleEnabledRequested)
            listener->enabled = !listener->enabled;
        const bool cancelled = IsInspectorCardEscapeRequested();
        if (!card.BeginBody())
            return {cancelled, header.resetRequested || header.toggleEnabledRequested, header.removeRequested};
        ImGui::BeginDisabled(!listener->enabled);
        const auto fields = InspectorFields::Draw(*listener, context);
        ImGui::EndDisabled();
        card.Finish();
        return {cancelled, fields.committed || header.resetRequested || header.toggleEnabledRequested, header.removeRequested};
    }

    /** @copydoc InspectorPanel::DrawAudioPreview */
    void InspectorPanel::DrawAudioPreview(const SceneObject &object, const EditorWorkspaceViewModel &model,
                                          EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        const bool cleanDraft = m_editSession.Draft().audioSource == object.components.audioSource;
        const bool canSubmit = command.command == EditorWorkspaceViewCommand::None;
        if (Ui::Button({.label = Label(context, "workspace.inspector.audio.preview"),
                        .enabled = canSubmit && cleanDraft && object.components.audioSource->enabled && m_editSession.IsAudioSourceValid(),
                        .font = context.theme.fonts.sans})) {
            command.command = EditorWorkspaceViewCommand::StartAudioComponentPreview;
            command.objectPayload = object.id;
            command.audioPreviewRevision = model.documentRevision;
        }
        if (Ui::Button({.label = Label(context, "workspace.inspector.audio.stop"),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = canSubmit,
                        .font = context.theme.fonts.sans}))
            command.command = EditorWorkspaceViewCommand::StopAudioComponentPreview;
        const char *message = nullptr;
        if (!model.audioPreviewAvailable)
            message = "workspace.inspector.audio.provider_missing";
        else if (model.audioPreviewFailure)
            message = PreviewFailureMessage(*model.audioPreviewFailure);
        if (!message && model.audioPreview && model.audioPreview->output)
            ImGui::TextWrapped("%s", Label(context, PreviewPhaseMessage(model.audioPreview->output->phase)));
        if (message)
            DrawValidationMessageIfInvalid(false, context.localization.Get("editor", message), context.theme.fonts, 8.0F);
    }
}  // namespace Horo::Editor
