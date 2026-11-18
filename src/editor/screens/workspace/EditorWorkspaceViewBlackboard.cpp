#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/LocalizedGui.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <array>

namespace Horo::Editor {
    namespace {
        constexpr GuiLocalizationScope BlackboardText{"workspace.blackboard"};

        /** @brief Borrows presentation services for the active immutable document. */
        struct BlackboardFrame final {
            const ILocalizationService &localization;
            const Theme::Fonts &fonts;
            BlackboardDocumentFence fence;
            EditorWorkspaceViewCommandData &command;

            const std::string &Text(const char *key) const {
                return BlackboardText.Text(localization, key);
            }

            void Caption(const char *key) const {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(key), Theme::Text(), fonts);
            }

            bool Button(const char *key, bool enabled = true) const {
                return Ui::Button({.label = Text(key).c_str(),
                                   .variant = Ui::ButtonVariant::Secondary,
                                   .enabled = enabled,
                                   .font = fonts.sans,
                                   .style = {.width = Ui::StyleWidth::FillAvailable}});
            }

            void Request(BlackboardDocumentOperation operation, std::optional<BlackboardSchemaEdit> edit = {}) const {
                if (command.command != EditorWorkspaceViewCommand::None)
                    return;
                command.command = EditorWorkspaceViewCommand::BlackboardDocument;
                command.blackboard = BlackboardWorkspaceRequest{.fence = fence, .operation = operation, .edit = std::move(edit)};
            }

            /** @brief Renders a typed enum using an exhaustive locale label array. */
            template <class Enum, std::size_t Count>
            bool Choice(const char *label, Enum &value, const std::array<const char *, Count> &keys, bool enabled) const {
                static_assert(Count == static_cast<std::size_t>(Enum::Count));
                Caption(label);
                if (!enabled) {
                    Caption(keys[static_cast<std::size_t>(value)]);
                    return false;
                }
                std::array<const char *, Count> labels;
                for (std::size_t index = 0; index < Count; ++index)
                    labels[index] = Text(keys[index]).c_str();
                int index = static_cast<int>(value);
                if (!Ui::ComboControl(label, &index, labels.data(), static_cast<int>(labels.size()), fonts))
                    return false;
                value = static_cast<Enum>(index);
                return true;
            }

            void Edit(BlackboardSchemaEdit edit) const {
                Request(BlackboardDocumentOperation::Edit, std::move(edit));
            }
        };

        /** @brief Renders descriptor fields while leaving durable semantics under the document's reference fence. */
        void DrawTypeFields(const BlackboardFrame &frame, const AI::BlackboardKeyDescriptor &current, bool draft) {
            auto changed = current;
            if (frame.Choice("type", changed.kind, std::to_array({"bool", "integer", "scalar", "entity", "asset", "coordinate"}), draft)) {
                changed.defaultValue.reset();
            }
            if (frame.Choice("shape", changed.cardinality, std::to_array({"single", "collection"}), draft)) {
                changed.maximumCollectionElements =
                    changed.cardinality == AI::BlackboardValueCardinality::Scalar ? 1 : AI::MaximumBlackboardCollectionElements;
                changed.defaultValue.reset();
            }
            static_cast<void>(frame.Choice("access", changed.access, std::to_array({"read_only", "read_write"}), draft));
            static_cast<void>(frame.Choice("presence", changed.presence, std::to_array({"required", "optional"}), draft));
            if (changed.defaultValue)
                frame.Caption("default_set");
            if (draft && frame.Button("clear_default", changed.defaultValue.has_value()))
                changed.defaultValue.reset();
            if (changed != current)
                frame.Edit(EditBlackboardKey{std::move(changed)});
        }

        /** @brief Shows actionable source conflicts alongside shared save and history controls. */
        void DrawSourceTools(const BlackboardFrame &frame, const BlackboardWorkspaceDocument &document) {
            const auto &snapshot = document.snapshot;
            using enum BlackboardDocumentOperation;
            if (frame.Button("save", snapshot.dirty && !snapshot.conflict))
                frame.Request(Save);
            if (frame.Button("reload", !snapshot.dirty))
                frame.Request(Reload);
            if (frame.Button("undo", snapshot.canUndo))
                frame.Request(Undo);
            if (frame.Button("redo", snapshot.canRedo))
                frame.Request(Redo);
            if (snapshot.conflict)
                Ui::ErrorText(frame.Text("conflict").c_str(), frame.fonts);
            else if (document.diagnostic)
                Ui::ErrorText(frame.Text("operation_failed").c_str(), frame.fonts);
        }
    }  // namespace

    /** @copydoc EditorWorkspaceView::DrawBlackboardDocument */
    bool EditorWorkspaceView::DrawBlackboardDocument(const EditorWorkspaceViewModel &viewModel,
                                                     EditorWorkspaceViewCommandData &outCommand) const {
        const auto active = viewModel.workspacePanelHost.ActiveDocument();
        const auto document = std::ranges::find_if(viewModel.blackboardDocuments, [&](const auto &entry) {
            return entry.snapshot.document.instance == active;
        });
        if (document == viewModel.blackboardDocuments.end())
            return false;
        const auto &snapshot = document->snapshot;
        const auto &rows = snapshot.source->rows;
        const BlackboardFrame frame{m_context.localization,
                                    m_context.theme.fonts,
                                    {snapshot.document.instance, snapshot.revision},
                                    outCommand};
        auto &draft = m_blackboardDraft;
        if (draft.fence.instance != frame.fence.instance)
            draft.selected = {};
        const auto selected = std::ranges::find_if(rows, [&](const BlackboardSchemaRow &entry) {
            return entry.descriptor.key == draft.selected;
        });
        int rowIndex = selected == rows.end() ? 0 : static_cast<int>(selected - rows.begin());
        if (draft.fence.instance != frame.fence.instance || draft.fence.revision != frame.fence.revision) {
            draft.fence = frame.fence;
            draft.rowIds.clear();
            for (const auto &row : rows)
                draft.rowIds.push_back(std::to_string(row.descriptor.key.Value()));
            draft.name = rows.empty() ? "" : rows[static_cast<std::size_t>(rowIndex)].displayName.value_or("");
        }

        DrawSourceTools(frame, *document);
        if (frame.Button("add", snapshot.nextKey.has_value() && rows.size() < AI::MaximumBlackboardKeys))
            frame.Edit(AddBlackboardKey{{.descriptor = {.key = *snapshot.nextKey}}, rows.size()});
        if (rows.empty()) {
            frame.Caption("no_keys");
            return true;
        }

        frame.Caption("keys");
        const Ui::ComboItemSource display{.label = [&](int index) {
            const auto slot = static_cast<std::size_t>(index);
            const auto &row = rows[slot];
            return row.displayName ? row.displayName->c_str() : draft.rowIds[slot].c_str();
        }};
        if (Ui::ComboControl("BlackboardKey", &rowIndex, static_cast<int>(rows.size()), display, frame.fonts))
            draft.name = rows[static_cast<std::size_t>(rowIndex)].displayName.value_or("");
        const auto index = static_cast<std::size_t>(rowIndex);
        const auto &row = rows[index];
        draft.selected = row.descriptor.key;
        ImGui::PushID(draft.rowIds[index].c_str());
        frame.Caption("name");
        const bool rename = Ui::InputTextControl("BlackboardName", draft.name, AI::BlackboardSchemaAuthoringRow::MaximumLabelBytes + 1,
                                                 frame.fonts, {.submitOnEnter = true});
        outCommand.sourceTextInputOwned = outCommand.sourceTextInputOwned || ImGui::IsItemActive();
        if (frame.Button("apply_name", AI::IsValidBlackboardDisplayName(draft.name)) || rename)
            frame.Edit(RenameBlackboardKey{draft.selected, draft.name});
        if (frame.Button("move_up", index > 0))
            frame.Edit(MoveBlackboardKey{draft.selected, index - 1});
        if (frame.Button("move_down", index + 1 < rows.size()))
            frame.Edit(MoveBlackboardKey{draft.selected, index + 1});

        const auto durable = snapshot.durableSchema->Keys();
        const bool draftKey = !std::ranges::binary_search(durable, draft.selected, {}, &AI::BlackboardKeyDescriptor::key);
        if (draftKey) {
            if (frame.Button("remove"))
                frame.Edit(RemoveBlackboardKey{draft.selected});
        } else {
            frame.Caption("reference_required");
        }
        DrawTypeFields(frame, row.descriptor, draftKey);
        ImGui::PopID();
        return true;
    }
}  // namespace Horo::Editor
