#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/ExtensionManager.h"

namespace Horo::Extensions {
    /** @copydoc ExtensionManager::ReportEditorProviderStatus */
    Result<void> ExtensionManager::ReportEditorProviderStatus(const ExtensionManifest &manifest,
                                                              const EditorSurfaceProviderStatus status) const {
        if (!m_editorActivityHost)
            return Result<void>::Success();
        if (status == EditorSurfaceProviderStatus::Active || status == EditorSurfaceProviderStatus::Missing)
            return m_editorActivityHost->Registry().SetExtensionProviderStatus(manifest.id, status);
        for (const auto &module : manifest.modules) {
            if (auto reported = m_editorActivityHost->Registry().SetProviderStatus({manifest.id, module.id}, status); reported.HasError())
                return reported;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Extensions
