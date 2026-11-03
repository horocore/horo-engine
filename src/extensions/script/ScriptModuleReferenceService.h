#pragma once
#include "Horo/Extensions/BackendServiceRegistry.h"
#include "Horo/Extensions/ScriptInvocation.h"

#include <atomic>

namespace Horo::Extensions {
    /** Genuine builtin provider contract; publication uses the composition's existing registries and actual code lease. */
    class ScriptModuleReferenceService final {
    public:
        explicit ScriptModuleReferenceService(ScriptExportDescriptorSnapshotPtr descriptors) : descriptors_(std::move(descriptors)) {}

        [[nodiscard]] Result<ScriptCallResult> Invoke(const ScriptInvocationRequest &, const BackendServiceCallContext &) const;

        void Shutdown() noexcept {
            closed_.store(true, std::memory_order_release);
        }

    private:
        [[nodiscard]] Result<void> Validate(const ScriptInvocationRequest &) const;
        ScriptExportDescriptorSnapshotPtr descriptors_;
        std::atomic<bool> closed_{};
    };

    [[nodiscard]] Result<ScriptExportDescriptorSnapshotPtr> BuildScriptModuleReferenceApi(std::string module,
                                                                                          ScriptExportVersion version = {1, 0, 0});
}  // namespace Horo::Extensions
