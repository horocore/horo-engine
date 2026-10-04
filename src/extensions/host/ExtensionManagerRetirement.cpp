#include "../capabilities/asset_pipeline_points/ExternalAssetImporter.h"
#include "Horo/Assets/AssetImporter.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Foundation/Logging/Logger.h"

#include <algorithm>
#include <ranges>

namespace Horo::Extensions {
    /** @copydoc ExtensionManager::~ExtensionManager */
    ExtensionManager::~ExtensionManager() {
        CloseForDestruction();
    }

    /** @copydoc ExtensionManager::HasActivationCapacity */
    bool ExtensionManager::HasActivationCapacity(const std::size_t providerCount) const noexcept {
        return m_loadedExtensions.size() < 1024 && providerCount <= 1024;
    }

    /** @brief Admits complete provider closures before activating any dependent native module. */
    Result<void> ExtensionManager::AcquireProviderDependencies(const std::span<const std::string> providers,
                                                               const std::string_view extensionId,
                                                               std::vector<std::shared_ptr<ExtensionModuleLifetime>> &dependencies,
                                                               std::vector<std::shared_ptr<ExtensionExecutableLease>> &leases) const {
        for (const auto &provider : providers) {
            const auto found = m_loadedExtensions.find(provider);
            if (found == m_loadedExtensions.end() || found->second->retiring)
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::LoadFailed, "Extension provider must be activated before its dependent: " + provider));
            const auto &record = *found->second;
            for (std::size_t moduleIndex = 0; moduleIndex < record.moduleIds.size(); ++moduleIndex) {
                auto lease = record.retirement->Acquire(record.moduleIds[moduleIndex], ExtensionLeaseKind::HostService,
                                                        "dependent-package:" + std::string{extensionId}, record.lifetimes[moduleIndex]);
                if (!lease)
                    return Result<void>::Failure(
                        MakeError(ExtensionErrors::LoadFailed, "Provider dependency admission closed: " + provider));
                leases.push_back(std::move(lease));
                dependencies.push_back(record.lifetimes[moduleIndex]);
            }
        }
        return Result<void>::Success();
    }

    void ExtensionManager::UnloadExtension(const std::string &extensionId) {
        (void)RetireExtension(extensionId);
    }

    /** @copydoc ExtensionManager::Retirement */
    std::shared_ptr<ExtensionRetirement> ExtensionManager::Retirement(const std::string &extensionId) const {
        const auto found = m_loadedExtensions.find(extensionId);
        return found == m_loadedExtensions.end() ? nullptr : found->second->retirement;
    }

    /** @copydoc ExtensionManager::RetireExtension */
    ExtensionRetirementReport ExtensionManager::RetireExtension(const std::string &extensionId) {
        const auto retirement = Retirement(extensionId);
        if (!retirement)
            return {.extensionId = extensionId, .disposition = ExtensionRetirementDisposition::Complete};
        RetireRecord(extensionId);
        FinalizeRetirements();
        return retirement->Inspect();
    }

    /** @brief Withdraws dependents before their providers; native records remain until owner-lane finalization. */
    void ExtensionManager::RetireRecord(const std::string &extensionId) {
        if (const auto found = m_loadedExtensions.find(extensionId); found != m_loadedExtensions.end()) {
            LoadedExtension &extension = *found->second;
            if (extension.retiring)
                return;
            for (auto dependent = m_activationOrder.rbegin(); dependent != m_activationOrder.rend(); ++dependent) {
                const auto record = m_loadedExtensions.find(*dependent);
                if (record != m_loadedExtensions.end() &&
                    std::ranges::find(record->second->providerExtensions, extensionId) != record->second->providerExtensions.end())
                    RetireRecord(*dependent);
            }
            extension.retirement->CloseAdmission();
            extension.platformProvider.reset();
            if (m_importerCatalog && !m_importerCatalog->WithdrawPackage(extensionId))
                extension.retirement->RequireRestart();
            extension.retiring = true;
        }
    }

    /** @copydoc ExtensionManager::FinalizeRetirements */
    void ExtensionManager::FinalizeRetirements() {
        for (auto id = m_activationOrder.rbegin(); id != m_activationOrder.rend(); ++id) {
            const auto found = m_loadedExtensions.find(*id);
            if (found == m_loadedExtensions.end() || !found->second->retiring || !found->second->retirement->IsDrained())
                continue;
            while (!found->second->lifetimes.empty())
                found->second->lifetimes.pop_back();
            if (found->second->retirement->IsDrained()) {
                found->second->providerLeases.clear();
                m_loadedExtensions.erase(found);
            }
        }
        std::erase_if(m_activationOrder, [this](const auto &id) {
            return !m_loadedExtensions.contains(id);
        });
    }

    /** @brief Allocation-free destructor safety net; busy native records are intentionally retained until process restart. */
    void ExtensionManager::CloseForDestruction() noexcept {
        for (auto id = m_activationOrder.rbegin(); id != m_activationOrder.rend(); ++id) {
            const auto found = m_loadedExtensions.find(*id);
            if (found == m_loadedExtensions.end())
                continue;
            auto &extension = found->second;
            extension->retirement->CloseAdmission();
            extension->platformProvider.reset();
            if (m_importerCatalog && !m_importerCatalog->WithdrawPackage(*id))
                extension->retirement->RequireRestart();
            if (extension->retirement->IsDrained()) {
                while (!extension->lifetimes.empty())
                    extension->lifetimes.pop_back();
            }
            if (extension->retirement->IsDrained()) {
                extension->providerLeases.clear();
            } else {
                extension->retirement->RequireRestart();
                LOG_WARN("extensions", "Restart required: outstanding extension work retains native code for %s.", id->c_str());
                // No allocation or last-lease worker-thread unload is permitted during host teardown.
                // The 1024-record limit is per manager, not a global budget. Composition must honor
                // restart-required and must not repeatedly recreate failed hosts in this process.
                (void)extension.release();
            }
        }
    }

    void ExtensionManager::UnloadAll() {
        for (auto id = m_activationOrder.rbegin(); id != m_activationOrder.rend(); ++id)
            RetireRecord(*id);
        FinalizeRetirements();
    }

    std::vector<std::string> ExtensionManager::GetLoadedExtensionIds() const {
        std::vector<std::string> ids;
        ids.reserve(m_loadedExtensions.size());
        for (const auto &[key, extension] : m_loadedExtensions) {
            if (!extension->retiring)
                ids.push_back(key);
        }
        std::ranges::sort(ids);
        return ids;
    }
}  // namespace Horo::Extensions
