#include "ExtensionRegistrationRetirement.h"
#include "Horo/Extensions/EditorCommandRegistry.h"
#include "Horo/Extensions/EditorSurfaceRegistry.h"

namespace Horo::Extensions {
    /** @copydoc EditorCommandRegistration::AttachRetirement */
    bool EditorCommandRegistration::AttachRetirement(const std::shared_ptr<ExtensionRetirement> &retirement) {
        if (!retirement || !IsRegistered() || retirementLease_)
            return false;
        const auto module = RetirementModule();
        auto lease = retirement->Acquire(module, ExtensionLeaseKind::UiSurface, Id().value, retirement);
        if (!lease)
            return false;
        auto owner =
            std::make_shared<Detail::RetirementRegistration<EditorCommandRegistration>>(EditorCommandRegistration{registry_, entry_});
        if (!retirement->RegisterContribution(module, std::move(owner)))
            return false;
        retirementLease_ = std::move(lease);
        return true;
    }

    /** @copydoc EditorSurfaceRegistration::AttachRetirement */
    bool EditorSurfaceRegistration::AttachRetirement(const std::shared_ptr<ExtensionRetirement> &retirement) {
        if (!retirement || !IsRegistered() || retirementLease_)
            return false;
        auto lease = retirement->Acquire(Descriptor().provider.moduleId, ExtensionLeaseKind::UiSurface, Descriptor().id, retirement);
        if (!lease)
            return false;
        auto owner =
            std::make_shared<Detail::RetirementRegistration<EditorSurfaceRegistration>>(EditorSurfaceRegistration{registry_, surface_});
        if (!retirement->RegisterContribution(Descriptor().provider.moduleId, std::move(owner)))
            return false;
        retirementLease_ = std::move(lease);
        return true;
    }
}  // namespace Horo::Extensions
