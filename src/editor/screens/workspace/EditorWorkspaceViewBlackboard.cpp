#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Application/BlackboardUsageReview.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/LocalizedGui.h"
#include "editor/screens/workspace/BlackboardScalarInput.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <array>
#include <format>

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

            void Label(std::string_view text) const {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, text, Theme::Text(), fonts);
            }

            void Caption(const char *key) const {
                Label(Text(key));
            }

            bool Input(const char *id, std::string &value, std::size_t capacity) const {
                const bool submitted = Ui::InputTextControl(id, value, capacity, fonts, {.submitOnEnter = true});
                command.sourceTextInputOwned = command.sourceTextInputOwned || ImGui::IsItemActive();
                return submitted;
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

        enum class BooleanDefault : std::uint8_t {
            Absent,
            False,
            True,
            Count
        };

        /** @brief Returns the incoming scalar Boolean if supplied by the admitted descriptor. */
        std::optional<bool> ReadBooleanDefault(const AI::BlackboardKeyDescriptor &current) {
            const auto *scalar = current.defaultValue ? std::get_if<AI::BlackboardScalarValue>(&*current.defaultValue) : nullptr;
            if (const auto *value = scalar ? std::get_if<bool>(scalar) : nullptr)
                return *value;
            return std::nullopt;
        }

        /** @brief Edits a Boolean default and its presence in one typed operation. */
        void DrawBooleanDefault(const BlackboardFrame &frame, AI::BlackboardKeyDescriptor &current, bool draft) {
            const auto existing = ReadBooleanDefault(current);
            auto selection = BooleanDefault::Absent;
            if (existing)
                selection = *existing ? BooleanDefault::True : BooleanDefault::False;
            if (!frame.Choice("default_value", selection, std::to_array({"default_none", "value_false", "value_true"}), draft))
                return;
            if (selection == BooleanDefault::Absent)
                current.defaultValue.reset();
            else
                current.defaultValue = AI::BlackboardScalarValue{selection == BooleanDefault::True};
        }

        /** @brief Keeps incomplete or non-finite text local until a complete numeric replacement is applied. */
        void DrawNumericDefault(const BlackboardFrame &frame, AI::BlackboardKeyDescriptor &current, std::string &text, bool draft) {
            frame.Caption("default_value");
            if (!draft) {
                if (current.defaultValue)
                    frame.Label(text);
                else
                    frame.Caption("default_none");
                return;
            }
            constexpr std::size_t MaximumNumericTextBytes = 64;
            const bool submitted = frame.Input("BlackboardNumericDefault", text, MaximumNumericTextBytes + 1);
            const auto candidate = ReadBlackboardNumericDefault(current.kind, text);
            const bool apply = frame.Button("apply_default", candidate.has_value()) || submitted;
            if (apply && candidate)
                current.defaultValue = *candidate;
            if (!text.empty() && !candidate)
                Ui::ErrorText(frame.Text("default_invalid").c_str(), frame.fonts);
            if (frame.Button("clear_default", current.defaultValue.has_value())) {
                current.defaultValue.reset();
                text.clear();
            }
        }

        /** @brief Selects the typed scalar default control. Other defaults stay inspectable and clearable. */
        void DrawDefaultField(const BlackboardFrame &frame, AI::BlackboardKeyDescriptor &current, std::string &text, bool draft) {
            using enum AI::BlackboardValueKind;
            if (current.cardinality == AI::BlackboardValueCardinality::Scalar) {
                if (current.kind == Boolean) {
                    DrawBooleanDefault(frame, current, draft);
                    return;
                }
                if (current.kind == SignedInteger || current.kind == Scalar) {
                    DrawNumericDefault(frame, current, text, draft);
                    return;
                }
            }
            frame.Caption(current.defaultValue ? "default_set" : "default_none");
            if (draft && frame.Button("clear_default", current.defaultValue.has_value()))
                current.defaultValue.reset();
        }

        /** @brief Bounds capacity without truncating an existing collection default. */
        void DrawCollectionCapacity(const BlackboardFrame &frame, AI::BlackboardKeyDescriptor &current, bool draft) {
            if (current.cardinality != AI::BlackboardValueCardinality::Collection)
                return;
            frame.Caption("collection_capacity");
            const auto capacity = static_cast<int>(current.maximumCollectionElements);
            if (!draft) {
                frame.Label(std::to_string(capacity));
                return;
            }
            const auto *initial = current.defaultValue ? std::get_if<AI::BlackboardCollectionValue>(&*current.defaultValue) : nullptr;
            const auto minimum = initial ? std::max(1, static_cast<int>(initial->count)) : 1;
            constexpr int maximum = static_cast<int>(AI::MaximumBlackboardCollectionElements);
            if (minimum == maximum) {
                frame.Label(std::to_string(capacity));
                return;
            }
            static const auto labels = [] {
                std::array<std::string, AI::MaximumBlackboardCollectionElements> result;
                for (std::size_t index = 0; index < result.size(); ++index)
                    result[index] = std::to_string(index + 1);
                return result;
            }();
            const Ui::ComboItemSource choices{.label = [minimum](int index) {
                return labels[static_cast<std::size_t>(index + minimum - 1)].c_str();
            }};
            int selection = capacity - minimum;
            if (Ui::ComboControl("BlackboardCollectionCapacity", &selection, maximum - minimum + 1, choices, frame.fonts))
                current.maximumCollectionElements = static_cast<std::size_t>(selection + minimum);
        }

        /** @brief Renders descriptor fields while leaving durable semantics under the document's reference fence. */
        void DrawTypeFields(const BlackboardFrame &frame, const AI::BlackboardKeyDescriptor &current, std::string &number, bool draft) {
            auto changed = current;
            const bool typeChanged =
                frame.Choice("type", changed.kind, std::to_array({"bool", "integer", "scalar", "entity", "asset", "coordinate"}), draft);
            const bool shapeChanged = frame.Choice("shape", changed.cardinality, std::to_array({"single", "collection"}), draft);
            if (shapeChanged)
                changed.maximumCollectionElements =
                    changed.cardinality == AI::BlackboardValueCardinality::Scalar ? 1 : AI::MaximumBlackboardCollectionElements;
            static_cast<void>(frame.Choice("access", changed.access, std::to_array({"read_only", "read_write"}), draft));
            static_cast<void>(frame.Choice("presence", changed.presence, std::to_array({"required", "optional"}), draft));
            if (typeChanged || shapeChanged) {
                changed.defaultValue.reset();
                number.clear();
            } else {
                DrawCollectionCapacity(frame, changed, draft);
                DrawDefaultField(frame, changed, number, draft);
            }
            if (changed != current)
                frame.Edit(EditBlackboardKey{std::move(changed)});
        }

        /** @brief Resolves the stable owner category without inspecting arbitrary authored property values. */
        const char *UseGapLabel(Application::BlackboardUsageGapKind kind) {
            using enum Application::BlackboardUsageGapKind;
            switch (kind) {
                case RegistryInventory:
                    return "usage_registry";
                case DecisionSource:
                    return "usage_decision_source";
                case DecisionProvider:
                    return "usage_decision_provider";
                case SceneSource:
                    return "usage_scene";
                case PrefabSource:
                    return "usage_prefab";
                case EnvironmentQueryBindings:
                    return "usage_queries";
                case PerceptionPolicy:
                    return "usage_perception";
            }
            return "operation_failed";
        }

        /** @brief Displays scoped review evidence for the selected durable key. */
        void DrawUsages(const BlackboardFrame &frame, const BlackboardWorkspaceDocument &document, AI::BlackboardKeyId key,
                        Assets::AssetRegistryRevision currentRegistry, std::size_t &page) {
            const auto &snapshot = document.snapshot;
            if (frame.Button("usage_review",
                             document.usageReviewAvailable && !document.reviewingUsages && !snapshot.dirty && !snapshot.conflict) &&
                frame.command.command == EditorWorkspaceViewCommand::None) {
                frame.command.command = EditorWorkspaceViewCommand::BlackboardDocument;
                frame.command.blackboard = BlackboardWorkspaceRequest{.fence = frame.fence,
                                                                      .operation = BlackboardDocumentOperation::ReviewUsages,
                                                                      .reviewKey = key};
                page = 0;
            }
            if (document.reviewingUsages)
                frame.Caption("usage_pending");
            const auto &review = document.usageReview;
            if (!review || review->selection.key != key)
                return;
            if (review->registry != currentRegistry) {
                frame.Caption("usage_stale");
                return;
            }
            frame.Caption(review->uses.empty() ? "usage_none" : "usage_found");
            if (!review->schemaBindings.empty())
                frame.Caption("usage_schema_bindings");
            if (!review->gaps.empty())
                frame.Caption("usage_incomplete");
            constexpr std::size_t RowsPerPage = 12;
            const auto bindingEnd = review->uses.size() + review->schemaBindings.size();
            const auto count = bindingEnd + review->gaps.size();
            if (!count)
                return;
            const auto lastPage = (count - 1) / RowsPerPage;
            page = std::min(page, lastPage);
            const auto start = page * RowsPerPage;
            const auto end = std::min(start + RowsPerPage, count);
            for (auto index = start; index < end; ++index) {
                if (index < review->uses.size()) {
                    const auto &usage = review->uses[index];
                    frame.Label(std::format("{} ({})", usage.sourcePath, usage.use.node.Value()));
                    continue;
                }
                if (index < bindingEnd) {
                    const auto &binding = review->schemaBindings[index - review->uses.size()];
                    const auto object = std::visit([](const auto &value) -> std::uint64_t {
                        return value.value;
                    }, binding.object);
                    frame.Label(std::format("{} ({})", binding.sourcePath, object));
                    continue;
                }
                const auto &gap = review->gaps[index - bindingEnd];
                frame.Caption(UseGapLabel(gap.kind));
                if (gap.asset)
                    frame.Label(gap.asset->ToString());
                if (gap.node)
                    frame.Label(std::to_string(gap.node->Value()));
            }
            if (lastPage) {
                frame.Label(std::format("{} / {}", page + 1, lastPage + 1));
                if (frame.Button("usage_previous", page > 0))
                    --page;
                if (frame.Button("usage_next", page < lastPage))
                    ++page;
            }
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
        const auto selectRow = [&](int index) {
            const auto &selectedRow = rows[static_cast<std::size_t>(index)];
            draft.selected = selectedRow.descriptor.key;
            draft.name = selectedRow.displayName.value_or("");
            draft.numericDefault = BlackboardNumericDefaultText(selectedRow.descriptor.defaultValue);
            draft.usagePage = 0;
        };
        if (draft.fence.instance != frame.fence.instance || draft.fence.revision != frame.fence.revision) {
            draft.fence = frame.fence;
            draft.rowIds.clear();
            for (const auto &row : rows)
                draft.rowIds.push_back(std::to_string(row.descriptor.key.Value()));
            if (!rows.empty())
                selectRow(rowIndex);
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
            selectRow(rowIndex);
        const auto index = static_cast<std::size_t>(rowIndex);
        const auto &row = rows[index];
        draft.selected = row.descriptor.key;
        ImGui::PushID(draft.rowIds[index].c_str());
        frame.Caption("name");
        const bool rename = frame.Input("BlackboardName", draft.name, AI::BlackboardSchemaAuthoringRow::MaximumLabelBytes + 1);
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
            DrawUsages(frame, *document, draft.selected, viewModel.assetRegistryRevision, draft.usagePage);
        }
        DrawTypeFields(frame, row.descriptor, draft.numericDefault, draftKey);
        ImGui::PopID();
        return true;
    }
}  // namespace Horo::Editor
