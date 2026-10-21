#pragma once
/** @file InspectorContributionView.h @brief Cached real-owner inspector projection through shared FormKit and editor controls. */
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Extensions/EditorInspectorHost.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <array>

namespace Horo::Editor {
    /** @brief Owner-lane view; host/theme outlive the panel and no provider executes in traversal. */
    class InspectorContributionView final {
    public:
        /** @brief Borrows explicit composed host and current immutable-theme view for this panel attachment. */
        InspectorContributionView(Extensions::EditorInspectorHost &host, const Extensions::EditorThemeFrame &theme);
        /** @brief Rebuilds only on actual owner/selection/provider revisions, before default controls decide their fallback coverage. */
        /** @brief Applies the real panel/workspace input decision and discards previews when ownership is withdrawn. */
        void SetInputAdmitted(bool admitted) noexcept;
        void Synchronize(const EditorWorkspaceViewModel &model, std::span<const SceneObjectId> selection);
        /** @brief Prepares validated current geometry before allowing custom fields to replace their generic controls.
         * @param attachment Primary attachment identity. @param context Current actual GUI context.
         * @return Admitted covered field names until Synchronize; empty means retain the complete generic inspector.
         */
        [[nodiscard]] std::span<const std::string> CoveredFields(Gameplay::BehaviorInstanceId attachment, const EditorGuiContext &context);
        /** @brief Draws one primary attachment's validated custom controls and queues only typed owner-fenced user intent. */
        void Draw(Gameplay::BehaviorInstanceId attachment, const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command,
                  const EditorGuiContext &context);
        /** @brief Draws actual common selected behavior schemas for multi-edit; ambiguous repeated attachments stay unsupported. */
        void DrawMulti(const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command, const EditorGuiContext &context);
        /** @brief Borrows current semantic hooks until synchronization; revoked providers expose no records. */
        [[nodiscard]] std::span<const Extensions::EditorUiAccessibleNode> Accessibility(std::string_view surface) const;

    private:
        struct Entry final {
            Extensions::EditorInspectorContribution contribution;
            Gameplay::BehaviorTypeId behavior;
            std::uint32_t schemaVersion{};
            std::vector<InspectorBehaviorTarget> targets;
            Extensions::EditorUiForm form;
            std::vector<std::string> covered;
            std::array<std::string, 256> buffers;
            std::array<bool, 256> mixed{};
            std::vector<std::pair<std::size_t, Extensions::EditorUiScalarState>> scalars;
            std::optional<Extensions::EditorUiRenderSnapshot> render;
            std::array<std::size_t, 256> order{};
            bool enabled{};
            bool approved{};
            Gameplay::BehaviorInstanceId primaryAttachment;
            std::optional<Error> failure;
        };

        /** @brief Copies and validates common actual owner values into a declaration; returns no entry on schema/type mismatch. */
        [[nodiscard]] std::optional<Entry> Capture(const Extensions::EditorInspectorContribution &contribution,
                                                   const Gameplay::BehaviorDescriptor &descriptor,
                                                   const Gameplay::BehaviorComponent &primary, const EditorWorkspaceViewModel &model,
                                                   std::span<const SceneObjectId> selection) const;
        /** @brief Validates one actual selected schema and copies exact/mixed scalar values into its admitted primitives. */
        [[nodiscard]] bool CaptureValues(Entry &entry, const Gameplay::BehaviorDescriptor &descriptor,
                                         const Gameplay::BehaviorComponent &behavior, const EditorWorkspaceViewModel &model) const;
        /** @brief Resolves real shared tokens/width into a cached validated snapshot; failure preserves generic presentation. */
        [[nodiscard]] bool Prepare(Entry &entry) const;
        /** @brief Draws one prepared field/text using only shared controls, preserving localized wrapping and exact integer text. */
        void DrawNode(Entry &entry, std::size_t index, const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command,
                      const EditorGuiContext &context);
        /** @brief Parses one gesture from a shared primitive; drafts remain view-local until explicit edit deactivation. */
        [[nodiscard]] std::optional<Extensions::EditorInspectorScalar> DrawValue(Entry &entry, std::size_t index,
                                                                                 const Extensions::EditorInspectorField &field,
                                                                                 const EditorGuiContext &context);
        /** @brief Uses the same admitted scalar binding with a host-owned custom presenter and retained local preview. */
        [[nodiscard]] std::optional<Extensions::EditorInspectorScalar> DrawScalar(Entry &entry, std::size_t index,
                                                                                  const Extensions::EditorInspectorField &field,
                                                                                  const EditorGuiContext &context);
        /** @brief Validates a typed interaction against copied constraints, then emits a complete owner-fenced command. */
        void Queue(Entry &entry, const Extensions::EditorInspectorField &field, Extensions::EditorInspectorScalar value,
                   const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command) const;
        /** @brief Emits explicit document-owner admission intent; unapproved declarations never replace generic controls. */
        void DrawApproval(const Entry &entry, const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command,
                          const EditorGuiContext &context) const;
        /** @brief Draws the complete admitted entry in resolved geometry order, including semantic parent labels. */
        void DrawEntry(Entry &entry, const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command,
                       const EditorGuiContext &context);
        /** @brief Resolves immutable provider text, with exact host locale/en-US fallback. */
        [[nodiscard]] const char *Text(const Entry &entry, const Extensions::EditorUiText &text, const EditorGuiContext &context) const;
        Extensions::EditorInspectorHost &host_;
        const Extensions::EditorThemeFrame &theme_;
        std::vector<Entry> entries_;
        bool inputAdmitted_{true};
        std::uint64_t documentRevision_{};
        std::uint64_t documentGeneration_{};
        std::uint64_t selectionRevision_{};
        std::uint64_t registryRevision_{};
        std::uint64_t behaviorRegistryRevision_{};
        std::uint64_t approvalRevision_{};

        /** @brief Per-attachment fallback coverage is rebuilt only on actual owner/catalog changes. */
        struct Coverage final {
            Gameplay::BehaviorInstanceId attachment;
            std::vector<std::string> fields;
        };

        std::vector<Coverage> coverage_;
    };
}  // namespace Horo::Editor
