#include "editor/screens/workspace/panels/hierarchy/HierarchyEditSession.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace Horo::Editor {
    /** @copydoc HierarchyEditSession::Close */
    void HierarchyEditSession::Close() noexcept {
        m_prefabEntryByNode.clear();
        m_prefabNodeByTarget.clear();
        m_inputs.clear();
        m_parentByNode.clear();
        m_visibleRows.clear();
        m_selectedObjects.clear();
        m_selectionAnchor.reset();
        m_prefabProjection.reset();
        m_model.ClearSelection();
        m_projectionInitialized = false;
        m_projectedRevision = {};
        m_handledRevealRevision = {};
    }

    /** @copydoc HierarchyEditSession::PrefabTarget */
    const PrefabHierarchyTarget *HierarchyEditSession::PrefabTarget(const HierarchyNodeId id) const noexcept {
        const auto *entry = PrefabEntry(id);
        return entry == nullptr ? nullptr : &entry->target;
    }

    /** @copydoc HierarchyEditSession::PrefabEntry */
    const PrefabHierarchyEntry *HierarchyEditSession::PrefabEntry(const HierarchyNodeId id) const noexcept {
        const auto found = m_prefabEntryByNode.find(id);
        return found == m_prefabEntryByNode.end() ? nullptr : found->second;
    }

    /** @copydoc HierarchyEditSession::PrefabCommand */
    EditorWorkspaceViewCommandData HierarchyEditSession::PrefabCommand(const HierarchyNodeId id, const PrefabHierarchyAction action) const {
        const auto *target = PrefabTarget(id);
        if (target == nullptr || !m_prefabProjection)
            return {};
        EditorWorkspaceViewCommandData result;
        result.command = EditorWorkspaceViewCommand::PrefabHierarchy;
        result.prefabHierarchy = PrefabHierarchyCommand{m_prefabProjection->documentSession, m_prefabProjection->publication,
                                                        m_prefabProjection->documentRevision, *target, action};
        return result;
    }

    /** @copydoc HierarchyEditSession::SynchronizePrefabSelection */
    void HierarchyEditSession::SynchronizePrefabSelection(const EditorWorkspaceViewModel &viewModel) {
        if (!viewModel.prefabSelection)
            return;
        const auto row = m_prefabNodeByTarget.find(*viewModel.prefabSelection);
        if (row == m_prefabNodeByTarget.end())
            return;
        static_cast<void>(m_model.Select(row->second));
        auto parent = ParentId(row->second);
        while (parent.has_value()) {
            static_cast<void>(m_model.SetExpanded(*parent, true));
            parent = ParentId(*parent);
        }
    }

    /** @copydoc HierarchyEditSession::PreparePrefabAliases */
    void HierarchyEditSession::PreparePrefabAliases(const EditorWorkspaceViewModel &viewModel) {
        auto prior = std::move(m_prefabNodeByTarget);
        m_prefabEntryByNode.clear();
        m_prefabNodeByTarget.clear();
        m_prefabProjection = viewModel.prefabHierarchy;
        if (!m_prefabProjection)
            return;
        std::unordered_set<HierarchyNodeId> occupied;
        occupied.reserve(viewModel.objects.size() + m_prefabProjection->entries.size());
        for (const auto &object : viewModel.objects)
            occupied.insert(object.id.value);
        for (const auto &entry : m_prefabProjection->entries) {
            const auto old = prior.find(entry.target);
            if (old != prior.end() && old->second != 0 && occupied.insert(old->second).second) {
                m_prefabNodeByTarget.try_emplace(entry.target, old->second);
                m_prefabEntryByNode.try_emplace(old->second, &entry);
            }
        }
        auto next = std::numeric_limits<HierarchyNodeId>::max();
        for (const auto &entry : m_prefabProjection->entries) {
            if (m_prefabNodeByTarget.contains(entry.target))
                continue;
            while (occupied.contains(next))
                --next;
            const auto alias = next--;
            occupied.insert(alias);
            m_prefabNodeByTarget.try_emplace(entry.target, alias);
            m_prefabEntryByNode.try_emplace(alias, &entry);
        }
    }

    /** @copydoc HierarchyEditSession::AppendPrefabRows */
    void HierarchyEditSession::AppendPrefabRows(const EditorWorkspaceViewModel &viewModel) {
        PreparePrefabAliases(viewModel);
        if (!m_prefabProjection)
            return;
        for (const auto &entry : m_prefabProjection->entries) {
            std::optional<HierarchyNodeId> parent;
            if (entry.parent)
                parent = m_prefabNodeByTarget.at(*entry.parent);
            else {
                const auto placement =
                    std::ranges::find(m_prefabProjection->sources->instances, entry.target.instance, [](const auto &value) {
                    return value.authored.instanceId;
                });
                if (placement != m_prefabProjection->sources->instances.end() && placement->authored.parent)
                    parent = placement->authored.parent->value;
            }
            const auto alias = m_prefabNodeByTarget.at(entry.target);
            m_inputs.push_back({.id = alias,
                                .parent = parent,
                                .name = entry.name,
                                .type = entry.target.member ? HierarchyNodeType::PrefabMember : HierarchyNodeType::PrefabInstance});
            m_parentByNode.try_emplace(alias, parent);
        }
    }
}  // namespace Horo::Editor
