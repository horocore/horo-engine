#pragma once
#include "Horo/Extensions/ExtensionManifest.h"

#include <set>
#include <span>

namespace Horo::Extensions {
    struct ExtensionModulePlan;
}

namespace Horo::Extensions::Detail {
    /** @brief Selects explicit roots plus declared and required service-provider dependencies without evaluating unrelated compatibility.
     */
    [[nodiscard]] Result<std::set<std::string, std::less<>>> SelectModuleClosure(const ExtensionManifest &manifest,
                                                                                 std::span<const std::string> roots);
    /** @brief Retains exact owning-host modules and their contributions after complete selected-closure validation. */
    [[nodiscard]] Result<void> NarrowOwningModulePlan(ExtensionManifest &manifest, ExtensionModulePlan &plan,
                                                      std::span<const std::string> selectedModules);
}  // namespace Horo::Extensions::Detail
