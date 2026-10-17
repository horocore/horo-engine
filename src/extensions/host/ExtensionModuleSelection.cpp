#include "ExtensionModuleSelection.h"

#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ExtensionModuleResolution.h"

#include <algorithm>

namespace Horo::Extensions::Detail {
    namespace {
        /** @brief Adds all providers of required imports to the selected module closure. */
        void SelectRequiredProviders(const ExtensionManifest &manifest, const ExtensionModuleManifest &selectedModule,
                                     std::set<std::string, std::less<>> &selected) {
            for (const auto &imported : selectedModule.imports) {
                if (!imported.required)
                    continue;
                for (const auto &provider : manifest.modules)
                    if (std::ranges::find(provider.exports, imported.service, &ExtensionServiceExportManifest::id) !=
                        provider.exports.end())
                        selected.insert(provider.id);
            }
        }
    }  // namespace

    /** @copydoc SelectModuleClosure */
    Result<std::set<std::string, std::less<>>> SelectModuleClosure(const ExtensionManifest &manifest,
                                                                   const std::span<const std::string> roots) {
        std::set<std::string, std::less<>> selected;
        for (const auto &id : roots) {
            if (std::ranges::find(manifest.modules, id, &ExtensionModuleManifest::id) == manifest.modules.end())
                return Result<std::set<std::string, std::less<>>>::Failure(
                    MakeError(ExtensionErrors::ModuleResolutionFailed, "Selected module is absent."));
            selected.insert(id);
        }
        std::size_t remainingPasses = manifest.modules.size();
        while (remainingPasses != 0) {
            --remainingPasses;
            const auto previous = selected.size();
            for (const auto &selectedModule : manifest.modules) {
                if (!selected.contains(selectedModule.id))
                    continue;
                selected.insert(selectedModule.dependencies.begin(), selectedModule.dependencies.end());
                SelectRequiredProviders(manifest, selectedModule, selected);
            }
            if (selected.size() == previous)
                break;
        }
        return Result<std::set<std::string, std::less<>>>::Success(std::move(selected));
    }

    /** @copydoc NarrowOwningModulePlan */
    Result<void> NarrowOwningModulePlan(ExtensionManifest &manifest, ExtensionModulePlan &plan,
                                        const std::span<const std::string> selectedModules) {
        if (!selectedModules.empty()) {
            for (const auto &id : selectedModules)
                if (std::ranges::find(plan.moduleIds, id) == plan.moduleIds.end())
                    return Result<void>::Failure(
                        MakeError(ExtensionErrors::ModuleResolutionFailed, "Selected owning-host module is not compatible."));
            for (std::size_t index = plan.moduleIds.size(); index-- > 0;)
                if (std::ranges::find(selectedModules, plan.moduleIds[index]) == selectedModules.end()) {
                    plan.moduleIds.erase(plan.moduleIds.begin() + static_cast<std::ptrdiff_t>(index));
                    plan.selectedEntries.erase(plan.selectedEntries.begin() + static_cast<std::ptrdiff_t>(index));
                }
            std::erase_if(manifest.contributions, [&selectedModules](const auto &contribution) {
                return std::ranges::find(selectedModules, contribution.owningModule) == selectedModules.end();
            });
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Extensions::Detail
