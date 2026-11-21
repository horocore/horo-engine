#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Application/BlackboardUsageReview.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/LocalizedGui.h"
#include "editor/screens/workspace/BlackboardKeyWidgets.h"
#include "editor/screens/workspace/BlackboardScalarInput.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <type_traits>

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

            void Request(BlackboardDocumentOperation operation, std::optional<BlackboardSchemaEdit> edit = {},
                         std::optional<AI::BlackboardKeyId> replacement = {},
                         std::shared_ptr<const DocumentRecoveryRecord> recovery = {}) const {
                if (command.command != EditorWorkspaceViewCommand::None)
                    return;
                command.command = EditorWorkspaceViewCommand::BlackboardDocument;
                command.blackboard = BlackboardWorkspaceRequest{.fence = fence,
                                                                .operation = operation,
                                                                .edit = std::move(edit),
                                                                .replacement = replacement,
                                                                .recovery = std::move(recovery)};
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

        /** @brief Projects a tentative descriptor through shared typed editing controls. */
        AI::BlackboardKeyDescriptor DrawTypeFields(const BlackboardFrame &frame, const AI::BlackboardKeyDescriptor &current,
                                                   std::string &number, bool draft) {
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
            return changed;
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
                case DecisionRetarget:
                    return "usage_retarget_blocked";
                case DecisionClosure:
                    return "usage_closure_blocked";
            }
            return "operation_failed";
        }

        /** @brief Formats the stable authored object address without relying on display names. */
        std::string BindingAddress(const Application::BlackboardSchemaBindingObject &object) {
            return std::visit([](const auto &value) -> std::string {
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, Application::BlackboardPlacedPrefabObject>) {
                    auto address = std::to_string(value.key.instance.Value());
                    for (const auto slot : value.key.object.NestedInstanceScope())
                        address += std::format("/{}", slot.value);
                    return address + std::format("/{}", value.key.object.SourceObject().value);
                } else {
                    return std::to_string(value.value);
                }
            }, object);
        }

        /** @brief Projects one row within the concatenated bounded review collections. */
        void DrawUsageRow(const BlackboardFrame &frame, const Application::BlackboardUsageReview &review, std::size_t index) {
            if (index < review.uses.size()) {
                const auto &usage = review.uses[index];
                frame.Label(std::format("{} ({})", usage.sourcePath, usage.use.node.Value()));
                return;
            }
            index -= review.uses.size();
            if (index < review.queryUsages.size()) {
                const auto &usage = review.queryUsages[index];
                const auto target = usage.queryAsset ? usage.queryPath : std::to_string(usage.use.output.query.Value());
                frame.Label(std::format("{} ({}) → {}", usage.decisionPath, usage.use.node.Value(), target));
                return;
            }
            index -= review.queryUsages.size();
            if (index < review.schemaBindings.size()) {
                const auto &binding = review.schemaBindings[index];
                frame.Label(std::format("{} ({})", binding.sourcePath, BindingAddress(binding.object)));
                return;
            }
            const auto &gap = review.gaps[index - review.schemaBindings.size()];
            frame.Caption(UseGapLabel(gap.kind));
            if (gap.asset)
                frame.Label(gap.asset->ToString());
            if (gap.node)
                frame.Label(std::to_string(gap.node->Value()));
            if (gap.diagnostic)
                frame.Label(gap.diagnostic->code.Value());
        }

        /** @brief Pages the captured rows without copying the usage collections. */
        void DrawUsagePage(const BlackboardFrame &frame, const Application::BlackboardUsageReview &review, std::size_t &page) {
            constexpr std::size_t RowsPerPage = 12;
            const auto count = review.uses.size() + review.queryUsages.size() + review.schemaBindings.size() + review.gaps.size();
            if (!count)
                return;
            const auto lastPage = (count - 1) / RowsPerPage;
            page = std::min(page, lastPage);
            const auto start = page * RowsPerPage;
            const auto end = std::min(start + RowsPerPage, count);
            for (auto index = start; index < end; ++index)
                DrawUsageRow(frame, review, index);
            if (lastPage) {
                frame.Label(std::format("{} / {}", page + 1, lastPage + 1));
                if (frame.Button("usage_previous", page > 0))
                    --page;
                if (frame.Button("usage_next", page < lastPage))
                    ++page;
            }
        }

        /** @brief Labels the evidence present in the current review. */
        void DrawUsageSummary(const BlackboardFrame &frame, const Application::BlackboardUsageReview &review) {
            if (!review.uses.empty())
                frame.Caption("usage_found");
            else if (review.queryUsages.empty())
                frame.Caption("usage_none");
            if (!review.queryUsages.empty())
                frame.Caption("usage_query_outputs");
            if (!review.schemaBindings.empty())
                frame.Caption("usage_schema_bindings");
            if (!review.gaps.empty())
                frame.Caption("usage_incomplete");
            if (review.selection.replacement) {
                frame.Caption("usage_retarget_candidates");
                frame.Label(std::to_string(review.decisionRetargets.size()));
                frame.Caption("usage_retarget_reviewed_destination");
                frame.Label(std::to_string(review.selection.replacement->Value()));
            }
            if (const auto &validation = review.decisionValidation) {
                frame.Caption("usage_closure_valid");
                frame.Label(std::to_string(validation->roots));
            }
        }

        /** @brief Lists compatible destinations in authored row order under the current clean document fence. */
        std::optional<AI::BlackboardKeyId> DrawRetargetDestination(const BlackboardFrame &frame, const BlackboardSchemaSource &source,
                                                                   const BlackboardSchemaRow &original, std::span<const std::string> rowIds,
                                                                   std::optional<AI::BlackboardKeyId> current) {
            std::array<std::size_t, AI::MaximumBlackboardKeys> entries{};
            std::size_t total = 1;
            int selected = 0;
            for (std::size_t row = 0; row < source.rows.size(); ++row) {
                const auto &key = source.rows[row].descriptor;
                if (!AI::IsCompatibleBlackboardReplacement(original.descriptor, key))
                    continue;
                if (key.key == current)
                    selected = static_cast<int>(total);
                entries[total++] = row;
            }
            if (total == 1)
                return {};
            frame.Caption("review_retarget_destination");
            const Ui::ComboItemSource choices{.label = [&](int index) {
                if (index == 0)
                    return frame.Text("review_retarget_none").c_str();
                const auto row = entries[static_cast<std::size_t>(index)];
                return source.rows[row].displayName ? source.rows[row].displayName->c_str() : rowIds[row].c_str();
            }};
            static_cast<void>(Ui::ComboControl("BlackboardRetargetTarget", &selected, static_cast<int>(total), choices, frame.fonts));
            if (!selected)
                return {};
            return source.rows[entries[static_cast<std::size_t>(selected)]].descriptor.key;
        }

        /** @brief Displays scoped review evidence for the selected durable key. */
        void DrawUsages(const BlackboardFrame &frame, const BlackboardWorkspaceDocument &document,
                        const AI::BlackboardKeyDescriptor &proposed, std::optional<AI::BlackboardKeyId> replacement,
                        Assets::AssetRegistryRevision currentRegistry, std::size_t &page) {
            const auto &snapshot = document.snapshot;
            const auto key = proposed.key;
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
            bool matchesProposal = false;
            if (const auto &preview = document.referencePreview) {
                const auto keys = preview->schema->Keys();
                const auto found = std::ranges::lower_bound(keys, key, {}, &AI::BlackboardKeyDescriptor::key);
                if (found == keys.end() || found->key != key) {
                    matchesProposal = review->selection.replacement == replacement;
                    frame.Caption("usage_preview_removal");
                } else if (*found == proposed) {
                    matchesProposal = true;
                    frame.Caption("usage_preview_change");
                }
            }
            DrawUsageSummary(frame, *review);
            DrawUsagePage(frame, *review, page);
            if (matchesProposal && review->commitEvidence && review->gaps.empty()) {
                frame.Caption("reference_apply_scope");
                if (!document.referenceEditReady)
                    frame.Caption("reference_apply_participants");
                if (frame.Button("reference_apply", document.referenceEditReady) &&
                    frame.command.command == EditorWorkspaceViewCommand::None) {
                    frame.command.command = EditorWorkspaceViewCommand::BlackboardDocument;
                    frame.command.blackboard = BlackboardWorkspaceRequest{.fence = frame.fence,
                                                                          .operation = BlackboardDocumentOperation::ApplyReviewedEdit,
                                                                          .expectedReview = review};
                }
            }
        }

        /** @brief Shows actionable source conflicts alongside shared save and history controls. */
        void DrawSourceTools(const BlackboardFrame &frame, const BlackboardWorkspaceDocument &document) {
            const auto &snapshot = document.snapshot;
            using enum BlackboardDocumentOperation;
            if (frame.Button(document.referenceEditStaged ? "reference_save" : "save", snapshot.dirty && !snapshot.conflict))
                frame.Request(Save);
            if (frame.Button("reload", !snapshot.dirty))
                frame.Request(Reload);
            if (snapshot.conflict && snapshot.dirty && frame.Button("reload_discard", !document.referenceEditStaged))
                frame.Request(ReloadDiscardingDraft);
            if (document.recovery) {
                if (frame.Button("restore_recovery", !snapshot.dirty && !snapshot.conflict))
                    frame.Request(RestoreRecovery, {}, {}, document.recovery);
                if (frame.Button("discard_recovery"))
                    frame.Request(DiscardRecovery, {}, {}, document.recovery);
            }
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

    /** @copydoc DrawBlackboardDraftKey */
    void DrawBlackboardDraftKey(const EditorGuiContext &context, AI::BlackboardKeyDescriptor &key, std::string &numericDefault,
                                EditorWorkspaceViewCommandData &command) {
        const BlackboardFrame frame{context.localization, context.theme.fonts, {}, command};
        key = DrawTypeFields(frame, key, numericDefault, true);
    }

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
        if (draft.fence.instance != frame.fence.instance) {
            draft.selected = {};
            draft.diagnosticKeyRequest = 0;
        }
        const auto selected = std::ranges::find_if(rows, [&](const BlackboardSchemaRow &entry) {
            return entry.descriptor.key == draft.selected;
        });
        int rowIndex = selected == rows.end() ? 0 : static_cast<int>(selected - rows.begin());
        const auto selectRow = [&](int index) {
            const auto &selectedRow = rows[static_cast<std::size_t>(index)];
            draft.selected = selectedRow.descriptor.key;
            draft.name = selectedRow.displayName.value_or("");
            draft.proposed = selectedRow.descriptor;
            draft.replacement.reset();
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
        if (draft.diagnosticKeyRequest != document->diagnosticKeyRequest) {
            draft.diagnosticKeyRequest = document->diagnosticKeyRequest;
            const auto target = std::ranges::find_if(rows, [&](const auto &row) {
                return row.descriptor.key == document->diagnosticKey;
            });
            if (target != rows.end()) {
                rowIndex = static_cast<int>(target - rows.begin());
                selectRow(rowIndex);
            }
        }

        DrawSourceTools(frame, *document);
        if (document->referenceEditStaged)
            return true;
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
            auto changed = DrawTypeFields(frame, row.descriptor, draft.numericDefault, true);
            if (changed != row.descriptor)
                frame.Edit(EditBlackboardKey{std::move(changed)});
        } else {
            frame.Caption("reference_required");
            const bool canReview = document->usageReviewAvailable && !document->reviewingUsages && !snapshot.dirty && !snapshot.conflict;
            draft.proposed = DrawTypeFields(frame, draft.proposed, draft.numericDefault, canReview);
            if (frame.Button("review_change", canReview && draft.proposed != row.descriptor)) {
                frame.Request(BlackboardDocumentOperation::ReviewUsages, EditBlackboardKey{draft.proposed});
                draft.usagePage = 0;
            }
            if (canReview && rows.size() > 1)
                draft.replacement = DrawRetargetDestination(frame, *snapshot.source, row, draft.rowIds, draft.replacement);
            if (frame.Button("review_removal", canReview && rows.size() > 1)) {
                frame.Request(BlackboardDocumentOperation::ReviewUsages, RemoveBlackboardKey{draft.selected}, draft.replacement);
                draft.usagePage = 0;
            }
            DrawUsages(frame, *document, draft.proposed, draft.replacement, viewModel.assetRegistryRevision, draft.usagePage);
        }
        ImGui::PopID();
        return true;
    }
}  // namespace Horo::Editor
