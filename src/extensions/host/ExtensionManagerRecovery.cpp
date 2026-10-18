#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/ExtensionManager.h"

#include <algorithm>

namespace Horo::Extensions {
    /** @copydoc ExtensionManager::ReportEditorProviderStatus */
    Result<void> ExtensionManager::ReportEditorProviderStatus(const ExtensionManifest &manifest, const EditorSurfaceProviderStatus status,
                                                              const std::span<const std::string> selectedModules) const {
        if (!m_editorHosts.activities)
            return Result<void>::Success();
        if (selectedModules.empty() && (status == EditorSurfaceProviderStatus::Active || status == EditorSurfaceProviderStatus::Missing))
            return m_editorHosts.activities->Registry().SetExtensionProviderStatus(manifest.id, status);
        std::vector<EditorSurfaceProviderKey> providers;
        providers.reserve(manifest.modules.size());
        for (const auto &providerModule : manifest.modules)
            if (selectedModules.empty() || std::ranges::find(selectedModules, providerModule.id) != selectedModules.end())
                providers.emplace_back(manifest.id, providerModule.id);
        return m_editorHosts.activities->Registry().SetProviderStatuses(providers, status);
    }
}  // namespace Horo::Extensions
