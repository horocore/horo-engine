#include "Horo/Editor/WorkspacePanelHost.h"

#include <algorithm>
#include <type_traits>
#include <variant>

namespace Horo::Editor {
    namespace {
        const ErrorDomainId WorkspaceDocumentDomain{"horo.editor.workspace_document"};

        LayoutNode MakeDefaultLayout() {
            auto left = std::make_unique<LayoutNode>(
                TabStackNode{.id = "workspace.left", .tabs = {"horo.hierarchy"}, .activeTab = "horo.hierarchy"});
            auto document = std::make_unique<LayoutNode>(
                TabStackNode{.id = "workspace.document", .tabs = {"horo.viewport"}, .activeTab = "horo.viewport"});
            auto right = std::make_unique<LayoutNode>(
                TabStackNode{.id = "workspace.right", .tabs = {"horo.inspector"}, .activeTab = "horo.inspector"});

            auto documentAndRight = std::make_unique<LayoutNode>(SplitNode{.id = "workspace.document_right",
                                                                           .axis = WorkspaceSplitAxis::Horizontal,
                                                                           .ratio = 0.72F,
                                                                           .first = std::move(document),
                                                                           .second = std::move(right)});

            return LayoutNode(SplitNode{.id = "workspace.root",
                                        .axis = WorkspaceSplitAxis::Horizontal,
                                        .ratio = 0.24F,
                                        .first = std::move(left),
                                        .second = std::move(documentAndRight)});
        }

        bool ContainsPanel(const LayoutNode &node, const std::string_view panelId) {
            return std::visit([&]<typename NodeValue>(const NodeValue &value) {
                using NodeType = std::decay_t<NodeValue>;
                if constexpr (std::is_same_v<NodeType, SplitNode>) {
                    return (value.first != nullptr && ContainsPanel(*value.first, panelId)) ||
                           (value.second != nullptr && ContainsPanel(*value.second, panelId));
                } else if constexpr (std::is_same_v<NodeType, TabStackNode>) {
                    return std::ranges::find(value.tabs, panelId) != value.tabs.end();
                } else {
                    return value.panel == panelId;
                }
            }, node.value);
        }

        bool SplitAround(LayoutNode &node, const std::string_view targetNodeId, const std::string_view panelId,
                         const WorkspacePanelHost::DropKind kind) {
            if (const bool isTarget = std::visit(
                    [&]<typename NodeValue>(const NodeValue &value) {
                return value.id == targetNodeId;
            }, node.value);
                isTarget) {
                const bool horizontal = kind == WorkspacePanelHost::DropKind::SplitLeft || kind == WorkspacePanelHost::DropKind::SplitRight;
                const bool panelFirst = kind == WorkspacePanelHost::DropKind::SplitLeft || kind == WorkspacePanelHost::DropKind::SplitTop;
                auto original = std::make_unique<LayoutNode>(std::move(node));
                auto panel = std::make_unique<LayoutNode>(
                    PanelNode{.id = std::string(targetNodeId) + ".panel." + std::string(panelId), .panel = std::string(panelId)});
                SplitNode split{.id = std::string(targetNodeId) + ".split." + std::string(panelId),
                                .axis = horizontal ? WorkspaceSplitAxis::Horizontal : WorkspaceSplitAxis::Vertical,
                                .ratio = 0.5F};
                if (panelFirst) {
                    split.first = std::move(panel);
                    split.second = std::move(original);
                } else {
                    split.first = std::move(original);
                    split.second = std::move(panel);
                }
                node = LayoutNode(std::move(split));
                return true;
            }

            return std::visit([&]<typename NodeValue>(NodeValue &value) {
                using NodeType = std::decay_t<NodeValue>;
                if constexpr (std::is_same_v<NodeType, SplitNode>) {
                    return (value.first != nullptr && SplitAround(*value.first, targetNodeId, panelId, kind)) ||
                           (value.second != nullptr && SplitAround(*value.second, targetNodeId, panelId, kind));
                }
                return false;
            }, node.value);
        }

        [[nodiscard]] auto FindDocumentTab(std::vector<WorkspaceDocumentTab> &tabs, const DocumentInstanceId instance) {
            return std::ranges::find_if(tabs, [instance](const WorkspaceDocumentTab &tab) {
                return tab.identity.instance == instance;
            });
        }

        [[nodiscard]] auto FindDocumentTab(const std::vector<WorkspaceDocumentTab> &tabs, const DocumentInstanceId instance) {
            return std::ranges::find_if(tabs, [instance](const WorkspaceDocumentTab &tab) {
                return tab.identity.instance == instance;
            });
        }

        [[nodiscard]] bool ContainsDocumentKey(const std::vector<SerializedDocumentOpenKey> &documents,
                                               const SerializedDocumentOpenKey &candidate) {
            return std::ranges::find(documents, candidate) != documents.end();
        }
    }  // namespace

    namespace WorkspaceDocumentErrors {
        const ErrorCodeDescriptor DirtyDocument{
            .domain = WorkspaceDocumentDomain,
            .code = ErrorCode{"editor.workspace_document.dirty"},
            .defaultSeverity = ErrorSeverity::Warning,
            .summary = "The workspace document has unsaved changes.",
            .remediationHint = "Save the document or explicitly discard its changes before closing the tab.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor DocumentUnknown{
            .domain = WorkspaceDocumentDomain,
            .code = ErrorCode{"editor.workspace_document.unknown"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The requested workspace document is not open.",
            .remediationHint = "Refresh the workspace tabs and retry the operation.",
            .retryable = false,
            .userActionable = true,
        };
    }  // namespace WorkspaceDocumentErrors

    WorkspacePanelHost::WorkspacePanelHost() {
        m_layout.root = MakeDefaultLayout();
    }

    /** @copydoc WorkspacePanelHost::AttachDocumentIdentityRegistry */
    void WorkspacePanelHost::AttachDocumentIdentityRegistry(DocumentIdentityRegistry &registry) noexcept {
        if (m_documentTabs.empty())
            m_documentRegistry_ = &registry;
    }

    WorkspaceLayoutOperationResult WorkspacePanelHost::OpenPanel(const std::string_view panelId, const std::string_view stackId) {
        using enum WorkspaceLayoutOperationCode;
        if (panelId.empty())
            return {UnknownPanel};
        auto *stack = m_layout.FindTabStack(stackId);
        if (stack == nullptr)
            return {UnknownStack};
        if (ContainsPanel(m_layout.root, panelId))
            return {UnknownPanel};
        stack->tabs.emplace_back(panelId);
        stack->activeTab = stack->tabs.back();
        return {};
    }

    WorkspaceLayoutOperationResult WorkspacePanelHost::MovePanel(const std::string_view panelId, const TabPlacement &placement) {
        return m_layout.MoveTab(panelId, placement);
    }

    WorkspaceLayoutOperationResult WorkspacePanelHost::ClosePanel(const std::string_view panelId) {
        return m_layout.CloseTab(panelId);
    }

    WorkspaceLayoutOperationResult WorkspacePanelHost::SetActiveTab(const std::string_view stackId, const std::string_view panelId) {
        using enum WorkspaceLayoutOperationCode;
        auto *stack = m_layout.FindTabStack(stackId);
        if (stack == nullptr)
            return {UnknownStack};
        if (std::ranges::find(stack->tabs, panelId) == stack->tabs.end()) {
            return {UnknownPanel};
        }
        stack->activeTab = std::string(panelId);
        return {};
    }

    WorkspaceLayoutOperationResult WorkspacePanelHost::DockPanel(const std::string_view panelId, const std::string_view targetNodeId,
                                                                 const DropKind kind) {
        using enum WorkspaceLayoutOperationCode;
        if (panelId.empty())
            return {UnknownPanel};
        if (m_layout.FindNode(targetNodeId) == nullptr)
            return {UnknownStack};

        const WorkspaceLayout backup = m_layout;
        const bool alreadyPresent = ContainsPanel(m_layout.root, panelId);
        if (kind == DropKind::TabCenter) {
            auto *stack = m_layout.FindTabStack(targetNodeId);
            if (stack == nullptr)
                return {UnknownStack};
            if (alreadyPresent) {
                const auto result = m_layout.MoveTab(panelId, TabPlacement{std::string(targetNodeId), std::nullopt});
                if (!result.Succeeded())
                    m_layout = backup;
                return result;
            }
            stack->tabs.emplace_back(panelId);
            stack->activeTab = stack->tabs.back();
            return {};
        }

        if (alreadyPresent && !m_layout.CloseTab(panelId).Succeeded()) {
            m_layout = backup;
            return {UnknownPanel};
        }
        if (!SplitAround(m_layout.root, targetNodeId, panelId, kind)) {
            m_layout = backup;
            return {UnknownStack};
        }
        return {};
    }

    /** @copydoc WorkspacePanelHost::OpenDocument */
    Result<DocumentOpenResult> WorkspacePanelHost::OpenDocument(const DocumentOpenKey &key) {
        const Result<SerializedDocumentOpenKey> serialized = SerializeDocumentOpenKey(key);
        if (serialized.HasError())
            return Result<DocumentOpenResult>::Failure(serialized.ErrorValue());

        const Result<DocumentOpenResult> opened = m_documentRegistry_->Open(key);
        if (opened.HasError())
            return opened;

        const DocumentIdentity identity = opened.Value().identity;
        if (FindDocumentTab(m_documentTabs, identity.instance) == m_documentTabs.end()) {
            m_documentTabs.push_back(WorkspaceDocumentTab{.identity = identity});
            if (!ContainsDocumentKey(m_layout.openDocuments, serialized.Value()))
                m_layout.openDocuments.push_back(serialized.Value());
        }
        m_activeDocument = identity.instance;
        return opened;
    }

    /** @copydoc WorkspacePanelHost::CloseDocument */
    Result<void> WorkspacePanelHost::CloseDocument(const DocumentInstanceId instance, const WorkspaceDocumentClosePolicy policy) {
        const auto tab = FindDocumentTab(m_documentTabs, instance);
        if (tab == m_documentTabs.end())
            return Result<void>::Failure(MakeError(WorkspaceDocumentErrors::DocumentUnknown));
        if (tab->dirty && policy == WorkspaceDocumentClosePolicy::RequireClean)
            return Result<void>::Failure(MakeError(WorkspaceDocumentErrors::DirtyDocument));

        const DocumentOpenKey key = tab->identity.key;
        if (const Result<void> closed = m_documentRegistry_->Close(instance); closed.HasError())
            return closed;

        const Result<SerializedDocumentOpenKey> serialized = SerializeDocumentOpenKey(key);
        if (serialized.HasValue()) {
            std::erase(m_layout.openDocuments, serialized.Value());
        }
        m_documentTabs.erase(tab);
        if (m_activeDocument == instance) {
            m_activeDocument = m_documentTabs.empty() ? std::nullopt : std::optional{m_documentTabs.front().identity.instance};
        }
        return Result<void>::Success();
    }

    /** @copydoc WorkspacePanelHost::SetDocumentDirty */
    Result<void> WorkspacePanelHost::SetDocumentDirty(const DocumentInstanceId instance, const bool dirty) {
        const auto tab = FindDocumentTab(m_documentTabs, instance);
        if (tab == m_documentTabs.end())
            return Result<void>::Failure(MakeError(WorkspaceDocumentErrors::DocumentUnknown));
        tab->dirty = dirty;
        return Result<void>::Success();
    }

    /** @copydoc WorkspacePanelHost::FocusDocument */
    Result<void> WorkspacePanelHost::FocusDocument(const DocumentInstanceId instance) {
        if (FindDocumentTab(m_documentTabs, instance) == m_documentTabs.end())
            return Result<void>::Failure(MakeError(WorkspaceDocumentErrors::DocumentUnknown));
        m_activeDocument = instance;
        return Result<void>::Success();
    }

    bool WorkspacePanelHost::SaveLayout(const std::filesystem::path &path, std::string *error) const {
        return WorkspaceLayoutPersistence::Save(path, m_layout, error);
    }

    bool WorkspacePanelHost::RestoreLayout(const std::filesystem::path &path, std::string *error) {
        auto restored = WorkspaceLayoutPersistence::Load(path, error);
        if (!restored.has_value())
            return false;

        const std::vector<WorkspaceDocumentTab> previousTabs = m_documentTabs;
        const std::optional<DocumentInstanceId> previousActive = m_activeDocument;
        const WorkspaceLayout previousLayout = m_layout;
        const auto previousActiveTab =
            previousActive.has_value() ? FindDocumentTab(previousTabs, previousActive.value()) : previousTabs.end();
        const std::optional<DocumentOpenKey> previousActiveKey =
            previousActiveTab != previousTabs.end() ? std::optional{previousActiveTab->identity.key} : std::nullopt;

        const auto restorePrevious = [&]() {
            std::vector<WorkspaceDocumentTab> restoredTabs;
            restoredTabs.reserve(previousTabs.size());
            for (const WorkspaceDocumentTab &tab : previousTabs) {
                const Result<DocumentOpenResult> reopened = m_documentRegistry_->Open(tab.identity.key);
                if (reopened.HasError())
                    continue;
                restoredTabs.push_back(WorkspaceDocumentTab{.identity = reopened.Value().identity, .dirty = tab.dirty});
            }
            m_layout = previousLayout;
            m_documentTabs = std::move(restoredTabs);
            m_activeDocument.reset();
            if (previousActiveKey.has_value()) {
                const auto active = std::ranges::find_if(m_documentTabs, [&](const WorkspaceDocumentTab &tab) {
                    return tab.identity.key == *previousActiveKey;
                });
                if (active != m_documentTabs.end())
                    m_activeDocument = active->identity.instance;
            }
            if (!m_activeDocument.has_value() && !m_documentTabs.empty())
                m_activeDocument = m_documentTabs.front().identity.instance;
        };

        for (const WorkspaceDocumentTab &tab : previousTabs) {
            if (const Result<void> closed = m_documentRegistry_->Close(tab.identity.instance); closed.HasError()) {
                if (error)
                    *error = closed.ErrorValue().message;
                restorePrevious();
                return false;
            }
        }

        std::vector<WorkspaceDocumentTab> restoredTabs;
        restoredTabs.reserve(restored->openDocuments.size());
        for (const SerializedDocumentOpenKey &serialized : restored->openDocuments) {
            const Result<DocumentOpenKey> key = DeserializeDocumentOpenKey(serialized);
            if (key.HasError()) {
                if (error)
                    *error = "workspace document identity is invalid";
                for (const WorkspaceDocumentTab &tab : restoredTabs)
                    static_cast<void>(m_documentRegistry_->Close(tab.identity.instance));
                restorePrevious();
                return false;
            }
            const Result<DocumentOpenResult> opened = m_documentRegistry_->Open(key.Value());
            if (opened.HasError()) {
                if (error)
                    *error = opened.ErrorValue().message;
                for (const WorkspaceDocumentTab &tab : restoredTabs)
                    static_cast<void>(m_documentRegistry_->Close(tab.identity.instance));
                restorePrevious();
                return false;
            }
            restoredTabs.push_back(WorkspaceDocumentTab{.identity = opened.Value().identity});
        }

        m_layout = std::move(*restored);
        m_documentTabs = std::move(restoredTabs);
        m_activeDocument = m_documentTabs.empty() ? std::nullopt : std::optional{m_documentTabs.front().identity.instance};
        return true;
    }

}  // namespace Horo::Editor
