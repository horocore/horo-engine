#include "ExtensionRegistrationRetirement.h"
#include "Horo/Extensions/EditorCommandRegistry.h"
#include "Horo/Extensions/EditorSurfaceRegistry.h"

namespace Horo::Extensions {
    /** @copydoc EditorCommandRegistration::AttachRetirement */
    bool EditorCommandRegistration::AttachRetirement(const std::shared_ptr<ExtensionRetirement> &retirement) {
        if (!retirement || !IsRegistered() || retirementLease_)
            return false;
        const auto owningModule = RetirementModule();
        auto lease = retirement->Acquire(owningModule, ExtensionLeaseKind::UiSurface, Id().value, retirement);
        if (!lease)
            return false;
        if (auto owner =
                std::make_shared<Detail::RetirementRegistration<EditorCommandRegistration>>(EditorCommandRegistration{registry_, entry_});
            !retirement->RegisterContribution(owningModule, std::move(owner)))
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
        if (auto owner =
                std::make_shared<Detail::RetirementRegistration<EditorSurfaceRegistration>>(EditorSurfaceRegistration{registry_, surface_});
            !retirement->RegisterContribution(Descriptor().provider.moduleId, std::move(owner)))
            return false;
        retirementLease_ = std::move(lease);
        return true;
    }
}  // namespace Horo::Extensions
