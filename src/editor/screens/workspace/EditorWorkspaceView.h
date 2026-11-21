#pragma once

#include "DecisionTaskPalette.h"
#include "ExtensionActivityView.h"
#include "ExtensionRecoveryView.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "UiCanvasDocumentView.h"
#include "WorkspaceSplitterInteraction.h"
#include "editor/screens/workspace/DecisionGraphCanvas.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/source_editor/SourceEditorSession.h"
#include "editor/source_editor/SourceEditorWidget.h"

#include <array>
#include <cstdint>
#include <imgui.h>
#include <memory>
#include <string>
#include <vector>

namespace Horo::Editor {
    struct GuiContentRegion;

    class EditorWorkspaceView final : public Input::IInputCaptureOwner {
    public:
        struct WorkspaceLayoutGeometry {
            ImVec2 display{};
            float curY{0.0F};
            float leftActivityW{0.0F};
            float rightActivityW{0.0F};
            float hierarchyW{0.0F};
            float inspectorW{0.0F};
            float centerW{0.0F};
            float bottomDockW{0.0F};
            float availableDockW{0.0F};
            float mainH{0.0F};
            float contentH{0.0F};
            float activityBarH{0.0F};
        };

        struct ActivityBarOptions {
            WorkspaceDockArea area;
            bool indicatorOnRight;
            bool allowDragSources;
        };

        struct ActivityBarGeometry {
            float cellX;
            float contentY;
            float cellWidth;
            float cellHeight;
            float cellGap;
            ImDrawList *drawList;
        };

        struct ActivityBarGroupParams {
            std::size_t groupIndex{0};
            const ActivityBarGroup &group;
            float groupTop{0.0F};
            float groupBottom{0.0F};
            ImVec2 pos{};
            ImVec2 size{};
            const ActivityBarGeometry &geometry;
            const ActivityBarOptions &options;
            bool draggingActivityItem{false};
        };

        EditorWorkspaceView(const EditorGuiContext &context, const WorkspacePanelRegistry &panelRegistry, std::uintptr_t logoTexture,
                            Input::InputRouter &inputRouter, Input::InputContextToken &workspaceInputContext,
                            Extensions::EditorActivityHost *activityHost = nullptr, IEditorGuiRenderer *renderer = nullptr);

        /** @brief Releases transient inspector state before the workspace composition is destroyed. */
        ~EditorWorkspaceView();

        void Draw(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand,
                  const GuiContentRegion &contentRegion);

        [[nodiscard]] std::optional<Extensions::EditorActivitySide> UpdateExtensionActivities() {
            m_extensions.SetInputAdmitted(m_inputRouter.IsContextActive(m_workspaceInputContext));
            m_extensions.Update();
            m_extensionRecovery.Update();
            return m_extensions.TakeNativePanelClear();
        }

        void OnInputCaptureCancelled(Input::CaptureCancellationReason reason) noexcept override;
        /** @brief Tears down source presentations when their project document owner leaves or is replaced. */
        void ResetSourceSession() noexcept;

        /** @brief Queues a review for its exact source tab on the next draw. */
        [[nodiscard]] bool StageSourceReview(std::shared_ptr<const SourceReviewOverlay> overlay) {
            return m_sourceSession.StageReview(std::move(overlay));
        }

        /** @brief Reads the exact selected range from the active source's existing presentation. */
        [[nodiscard]] std::optional<SourceContextRange> ReadSourceSelection(const SourceDocumentSnapshot &snapshot) const {
            return m_sourceSession.ReadSelection(snapshot);
        }

    private:
        ExtensionActivityView m_extensions;
        ExtensionRecoveryView m_extensionRecovery;
        const EditorGuiContext &m_context;
        UiCanvasDocumentView m_uiCanvasView;
        const WorkspacePanelRegistry &m_panelRegistry;
        std::uintptr_t m_logoTexture;
        Input::InputRouter &m_inputRouter;
        Input::InputContextToken &m_workspaceInputContext;
        mutable WorkspaceSplitterInteraction m_splitterInteraction;
        mutable Input::InputContextToken m_panelDragContext;
        mutable Input::PointerCaptureToken m_panelDragCapture;
        std::string m_panelDragCandidateId;

        [[nodiscard]] bool EnsurePanelDragCapture();
        [[nodiscard]] bool PanelDragEligible() const noexcept;

        /**
         * @brief Draws the shared activity/document panel drag source payload.
         * @param panelId Stable workspace panel identifier used as the payload.
         * @param panel Panel supplying the drag-preview label.
         */
        void DrawActivityPanelDragSource(const std::string &panelId, const std::shared_ptr<IWorkspacePanel> &panel);

        void DrawMenuBar(const ImVec2 &display, const EditorWorkspaceViewModel &viewModel,
                         EditorWorkspaceViewCommandData &outCommand) const;

        /** @brief Draws one startup recovery indication and emits its read-only review or authoring action. */
        void DrawRecoveryBar(const ImVec2 &pos, const ImVec2 &size, const EditorWorkspaceViewModel &viewModel, bool paired,
                             EditorWorkspaceViewCommandData &outCommand) const;

        void DrawExternalConflictBar(const ImVec2 &pos, const ImVec2 &size, EditorWorkspaceViewCommandData &outCommand) const;

        void DrawDockArea(WorkspaceDockArea area, const char *windowId, const ImVec2 &pos, const ImVec2 &size,
                          std::string_view activePanelId, const EditorWorkspaceViewModel &viewModel,
                          EditorWorkspaceViewCommandData &outCommand);
        /** @brief Draws one persistent document tab and emits its select or close command. */
        void DrawDocumentTab(const std::string &panelId, EditorWorkspaceViewCommandData &outCommand, bool active);
        bool DrawNavigationDocument(const EditorWorkspaceViewModel &viewModel) const;
        /** @brief Draws the host's ordered instance tabs using shared controls and typed lifecycle commands. */
        void DrawWorkspaceDocumentTabs(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand);
        /** @brief Presents the active immutable source; text editing belongs to the source widget adapter. */
        bool DrawSourceDocument(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) const;
        /** @brief Composes one view's tools and concrete widget without taking document mutation ownership. */
        void DrawSourceEditor(DocumentInstanceId instance, const SourceDocumentSnapshot &source, const SourceHistoryState &history,
                              const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) const;
        std::optional<DocumentInstanceId> m_lastPresentedDocument;

        mutable SourceEditorSession m_sourceSession;
        /** @brief Resolves theme-sized document chrome, including its overflow affordance. */
        [[nodiscard]] float DocumentTabHeight(WorkspaceDockArea area) const;
        /** @brief Draws either the active document or its explicitly focused native panel. */
        void DrawDockContent(WorkspaceDockArea area, const std::shared_ptr<IWorkspacePanel> &panel,
                             const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) const;
        /** @brief Draws the active registered PCG document using immutable source and workspace-only state. */
        bool DrawPCGDocument(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) const;
        /** @brief Projects the active Blackboard schema and emits revision-fenced source commands. */
        bool DrawBlackboardDocument(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) const;

        /** @brief Renders an already admitted immutable Environment Query source. */
        bool DrawEnvironmentQueryDocument(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) const;

        /** @brief Transient wide-value controls preserve invalid or as-yet-unapplied input. */
        struct EnvironmentQueryFields final {
            struct Distance final {
                AI::QueryStageId stage;
                std::uint64_t radius{};
                bool preferNearer{};
                std::uint64_t weight{};
                AI::QueryContextId center;
                std::vector<AI::QueryPointContextOption> centers;
            };

            EnvironmentQueryDocumentFence fence;
            std::uint64_t step{};
            std::uint64_t outer{};
            std::uint64_t inner{};
            std::uint64_t maximum{};
            bool annulus{};
            bool currentlySensed{};
            AI::QueryContextId center;
            std::vector<Distance> distances;
            std::uint64_t newDistanceRadius{};
            std::optional<AI::QueryPointVisibilityStageSettings> visibility;
            std::optional<AI::QueryPointNavigationStageSettings> navigation;
            Physics::PhysicsQueryChannelId queryChannel;
            bool preferPathCost{};
            double pathCostCeiling{100};
            std::uint64_t pathCostWeight{1};
            bool providerRoom{};
            std::string widgetId;

            /** @brief Checks all editable fields before narrowing user input. */
            [[nodiscard]] bool IsValid() const;
            /** @brief Checks the draft path cost scoring settings. */
            [[nodiscard]] bool IsPathScoringValid() const noexcept;
            /** @brief Converts the validated controls to an owning authoring request. */
            [[nodiscard]] AI::BuiltinPointQuerySettings Capture() const;
        };

        mutable EnvironmentQueryFields m_environmentQueryFields;
        /** @brief Draws the existing distance stages and returns the identity selected for removal. */
        [[nodiscard]] std::optional<AI::QueryStageId> DrawEnvironmentQueryDistanceStages(EnvironmentQueryFields &fields) const;
        /** @brief Draws external provider choices and emits at most one structural edit. */
        [[nodiscard]] EnvironmentQueryStageChange DrawEnvironmentQueryProviderStages(
            EnvironmentQueryFields &fields, std::span<const PhysicsQueryChannelChoice> channels) const;
        /** @brief Draws typed Perception controls against the current document revision. */
        void DrawPerceptionQuerySettings(const EnvironmentQueryWorkspaceDocument &document,
                                         EditorWorkspaceViewCommandData &outCommand) const;
        /** @brief Draws revision-fenced save and history commands shared by editable query kinds. */
        void DrawEnvironmentQueryHistory(const EnvironmentQueryWorkspaceDocument &document,
                                         EditorWorkspaceViewCommandData &outCommand) const;
        /** @brief Draws typed settings and emits a single revision-fenced edit/history action. */
        void DrawEnvironmentQuerySettings(const EnvironmentQueryWorkspaceDocument &document,
                                          std::span<const PhysicsQueryChannelChoice> channels,
                                          EditorWorkspaceViewCommandData &outCommand) const;
        /** @brief Presents the admitted semantic source without retaining a runtime plan. */
        bool DrawDecisionGraphDocument(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) const;

        struct BlackboardViewDraft final {
            BlackboardDocumentFence fence;
            std::uint64_t diagnosticKeyRequest{};
            AI::BlackboardKeyId selected;
            std::string name;
            std::string numericDefault;
            AI::BlackboardKeyDescriptor proposed;
            std::optional<AI::BlackboardKeyId> replacement;
            std::size_t usagePage{};
            std::vector<std::string> rowIds;
        };

        mutable BlackboardViewDraft m_blackboardDraft;

        /** @brief Uncommitted signed priority tied to one transition and document revision. */
        struct StateMachinePriorityDraft final {
            AI::DecisionNodeId transition;
            std::uint64_t revision{};
            std::int32_t value{};
            std::string widgetId;
        };

        /** @brief Transient selection by semantic identity, scoped to one open graph instance. */
        struct StateMachineSelection final {
            DocumentInstanceId instance;
            AI::DecisionNodeId state;
            AI::DecisionNodeId target;
            AI::DecisionNodeId transition;
            std::optional<StateMachinePriorityDraft> priority{};
        };

        mutable StateMachineSelection m_stateMachineSelection;
        mutable DecisionGraphCanvas m_decisionGraphCanvas;
        /** @brief Routes neutral canvas selection and link gestures to the ordinary document owner and controls. */
        void ApplyDecisionCanvasInteraction(const DecisionGraphWorkspaceDocument &document, const DecisionCanvasInteraction &interaction,
                                            EditorWorkspaceViewCommandData &outCommand) const;

        /** @brief Remembers the selected authored node and exact installed Task in one open tree. */
        struct BehaviorTreeSelection final {
            DocumentInstanceId instance;
            Assets::AssetId subtree;
            AI::DecisionNodeId node;
            DecisionTaskPaletteDraft palette;
        };

        mutable BehaviorTreeSelection m_behaviorTreeSelection;

        /** @brief Typed property drafts stay scoped to one exact document and catalog revision. */
        struct DecisionTaskPropertyDraft final {
            DocumentInstanceId instance;
            AI::DecisionNodeId node;
            std::uint64_t revision{};
            std::uint64_t catalogRevision{};
            AI::BehaviorTreePropertyId property;
            AI::BehaviorTreeNodeId reference;
            bool checked{};
            std::array<char, AI::BehaviorTreeAssetHardLimits::PropertyBytes + 1> text{};
            std::string fieldScope;

            /** @brief Initializes widget state from the effective value of a newly selected property. */
            void Select(const AI::BehaviorTreeProperty &effective);
        };

        struct DecisionTaskPropertyBinding final {
            AI::DecisionNodeId node;
            AI::DecisionNodeTypeId type;
            AI::DecisionVersionRange versions;
            std::span<const AI::BehaviorTreeProperty> properties;
        };

        struct DecisionTaskPropertyIntent final {
            DecisionTaskSelection task;
            DecisionTaskPropertyEdit property;
        };

        mutable DecisionTaskPropertyDraft m_behaviorTreeTaskPropertyDraft;
        mutable DecisionTaskPropertyDraft m_utilityTaskPropertyDraft;
        mutable std::array<DecisionTaskPropertyDraft, static_cast<std::size_t>(StateMachineTaskPhase::Count)> m_stateTaskPropertyDrafts;
        /** @brief Offers the installed Task's supported overrides through shared typed controls. */
        [[nodiscard]] std::optional<DecisionTaskPropertyIntent> DrawDecisionTaskPropertyEditor(
            const EditorWorkspaceViewModel &viewModel, const DecisionGraphWorkspaceDocument &document,
            const DecisionTaskPropertyBinding &binding, DecisionTaskPropertyDraft &draft, EditorWorkspaceViewCommandData &outCommand) const;
        /** @brief Draws the selected typed field and returns only an explicitly applied value. */
        [[nodiscard]] std::optional<AI::BehaviorTreePropertyValue> DrawDecisionTaskPropertyValue(
            const AI::BehaviorTreeProperty &effective, std::string_view label, std::size_t textLimit,
            std::span<const AI::BehaviorTreeNode> referenceNodes, DecisionTaskPropertyDraft &draft,
            EditorWorkspaceViewCommandData &outCommand) const;
        /** @brief Draws the current state's precise typed action phases in the property-only document route. */
        void DrawStateMachineTaskPropertyEditors(const EditorWorkspaceViewModel &viewModel, const DecisionGraphWorkspaceDocument &document,
                                                 const AI::StateMachineState &state, EditorWorkspaceViewCommandData &outCommand) const;

        /** @brief Widget selections for one exact task and graph revision. */
        struct BehaviorTreeQuerySelection final {
            DocumentInstanceId instance;
            AI::DecisionNodeId node;
            std::uint64_t revision{};
            std::optional<DecisionTaskSelection> task;
            Assets::AssetId source;
            AI::BlackboardKeyId key;
            std::string timeout;
        };

        mutable BehaviorTreeQuerySelection m_behaviorTreeQuerySelection;
        /** @brief Draws the saved query review and typed output controls for one Task. */
        void DrawBehaviorTreeQueryOutput(const EditorWorkspaceViewModel &viewModel, const DecisionGraphWorkspaceDocument &document,
                                         const AI::BehaviorTreeNode &node, EditorWorkspaceViewCommandData &outCommand,
                                         std::optional<DecisionTaskSelection> task = {}) const;
        /** @brief Draws the reviewed key and tick fields and returns a valid submitted timeout. */
        std::optional<std::uint32_t> DrawBehaviorTreeQueryFields(const BehaviorTreeQueryReview &review) const;

        /** @brief Bounded numeric text drafts and stable selection for a Utility document. */
        struct UtilityGraphSelection final {
            DocumentInstanceId instance;
            std::uint64_t revision{};
            AI::DecisionNodeId option;
            DecisionTaskPaletteDraft palette;
            std::optional<std::size_t> row;
            AI::UtilityConsideration score;
            std::array<std::string, 6> fields;
        };

        mutable UtilityGraphSelection m_utilityGraphSelection;
        /** @brief Projects saved Blackboard score inputs and emits typed Utility authoring commands. */
        void DrawUtilityGraph(const EditorWorkspaceViewModel &viewModel, const DecisionGraphWorkspaceDocument &document,
                              EditorWorkspaceViewCommandData &outCommand) const;
        /** @brief Projects supported tree topology and emits stable revision-fenced edits. */
        void DrawBehaviorTreeTopology(const EditorWorkspaceViewModel &viewModel, const DecisionGraphWorkspaceDocument &document,
                                      EditorWorkspaceViewCommandData &outCommand) const;
        /** @brief Projects the active immutable sequence through the shared timeline control. */
        bool DrawSequenceDocument(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) const;
        /** @brief Draws source-only controls and the isolated stage using shared themed components. */
        bool DrawPrefabDocument(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) const;
        /** @brief Draw the active real terrain asset through the existing viewport panel. */
        bool DrawTerrainDocument(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) const;
        /** @brief Refreshes transient widget drafts only when their immutable source publication changes. */
        void RefreshPrefabNameDrafts(const PrefabDocumentSnapshot &source) const;
        /** @brief Emits source-local edits from persistent bounded widget buffers. */
        void DrawPrefabSourceFields(const PrefabDocumentSnapshot &source, EditorWorkspaceViewCommandData &outCommand) const;
        mutable std::optional<PrefabDocumentFence> m_prefabNameFence;
        mutable std::string m_prefabNameScope;
        mutable std::vector<std::array<char, Prefab::MaximumPrefabObjectNameBytes + 1>> m_prefabNameDrafts;
        void DrawDocumentTabs(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand);
        /** @brief Calculates clamped dock geometry from native and extension visibility without mutating layout or submitting UI. */
        [[nodiscard]] WorkspaceLayoutGeometry CalculateWorkspaceGeometry(const EditorWorkspaceViewModel &viewModel, const ImVec2 &display,
                                                                         float activityBarH, float curY) const;
        void DrawMiddleAndBottomDocks(const WorkspaceLayoutGeometry &geo, const EditorWorkspaceViewModel &viewModel,
                                      EditorWorkspaceViewCommandData &outCommand);
        /** @brief Draws the bottom extension drawer or native full/split dock, preserving command publication order. */
        void DrawBottomDock(const WorkspaceLayoutGeometry &geo, const EditorWorkspaceViewModel &viewModel,
                            EditorWorkspaceViewCommandData &outCommand);
        void DrawWorkspaceDropTarget(const char *targetNodeId, const char *id, const ImVec2 &position, const ImVec2 &size,
                                     WorkspacePanelHost::DropKind kind, EditorWorkspaceViewCommandData &outCommand) const;

        void DrawActivityBar(const ImVec2 &pos, const ImVec2 &size, const WorkspacePanelRegistry &registry,
                             const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand,
                             ActivityBarOptions options);
        void DrawActivityBarGroup(const ActivityBarGroupParams &params, const EditorWorkspaceViewModel &viewModel,
                                  EditorWorkspaceViewCommandData &outCommand);
        /** @brief Draws native items followed by extension items in one clipped rail group and publishes any native-panel clear command. */
        void DrawCombinedActivityBarGroup(const ActivityBarGroupParams &params, const EditorWorkspaceViewModel &viewModel,
                                          EditorWorkspaceViewCommandData &outCommand);
        bool DrawActivityDropSlot(ActivityBarSlot slot, float y, bool draggingActivityItem, const ActivityBarGeometry &geometry,
                                  EditorWorkspaceViewCommandData &outCommand) const;
        float DrawActivityItem(const std::string &panelId, float y, const ActivityBarGeometry &geometry,
                               const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand,
                               ActivityBarOptions options, const std::shared_ptr<IWorkspacePanel> &panel);
    };
}  // namespace Horo::Editor
