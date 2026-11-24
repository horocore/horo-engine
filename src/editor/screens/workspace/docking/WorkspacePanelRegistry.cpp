#include "Horo/Editor/WorkspacePanelRegistry.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] bool TryGetAreaIndex(const WorkspaceDockArea area, std::size_t &index) noexcept {
            using enum WorkspaceDockArea;
            switch (area) {
                case Left:
                    index = 0;
                    return true;
                case Right:
                    index = 1;
                    return true;
                case Bottom:
                    index = 2;
                    return true;
                case Document:
                    index = 3;
                    return true;
                default:
                    return false;
            }
        }
    }  // namespace

    void WorkspacePanelRegistry::RegisterPanel(std::shared_ptr<IWorkspacePanel> panel) {
        if (!panel) {
            return;
        }

        // Registration changes must happen between workspace mounts so every panel
        // in the registry shares the same attach context.
        if (m_attached) {
            return;
        }

        if (const auto it = std::ranges::find_if(m_panels,
                                                 [&](const std::shared_ptr<IWorkspacePanel> &p) {
            return p->GetId() == panel->GetId();
        });
            it != m_panels.end()) {
            *it = std::move(panel);
            RebuildAreaIndex();
        } else {
            m_panels.push_back(std::move(panel));
            RebuildAreaIndex();
        }
    }

    const std::vector<std::shared_ptr<IWorkspacePanel>> &WorkspacePanelRegistry::GetPanelsForArea(const WorkspaceDockArea area) const {
        if (std::size_t index = 0; TryGetAreaIndex(area, index)) {
            return m_panelsByArea[index];
        }

        static const std::vector<std::shared_ptr<IWorkspacePanel>> empty;
        return empty;
    }

    const std::vector<std::shared_ptr<IWorkspacePanel>> &WorkspacePanelRegistry::GetAllPanels() const {
        return m_panels;
    }

    void WorkspacePanelRegistry::AttachAll(PanelContext &ctx) {
        if (m_attached) {
            return;
        }

        for (const auto &panel : m_panels) {
            panel->OnAttach(ctx);
        }
        m_attached = true;
    }

    void WorkspacePanelRegistry::DetachAll() {
        if (!m_attached) {
            return;
        }

        for (auto it = m_panels.rbegin(); it != m_panels.rend(); ++it) {
            (*it)->OnDetach();
        }
        m_attached = false;
    }

    void WorkspacePanelRegistry::Clear() {
        DetachAll();
        m_panels.clear();
        for (auto &panels : m_panelsByArea) {
            panels.clear();
        }
    }

    void WorkspacePanelRegistry::RebuildAreaIndex() {
        for (auto &panels : m_panelsByArea) {
            panels.clear();
        }

        for (const auto &panel : m_panels) {
            std::size_t index = 0;
            if (TryGetAreaIndex(panel->GetDefaultDockArea(), index)) {
                m_panelsByArea[index].push_back(panel);
            }
        }
    }
}  // namespace Horo::Editor
