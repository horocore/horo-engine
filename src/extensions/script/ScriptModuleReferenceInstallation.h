#pragma once
#include "Horo/Extensions/ScriptModuleRuntime.h"

namespace Horo::Extensions {
    /** Explicit builtin provider installed in the actual host authorities; not a native package or VM authority. */
    class ScriptModuleReferenceInstallation final {
    public:
        static Result<std::unique_ptr<ScriptModuleReferenceInstallation>> Create(std::shared_ptr<class ModuleScriptHostAuthority>,
                                                                                 ScriptCapabilityProfile);
        ~ScriptModuleReferenceInstallation();
        [[nodiscard]] Result<ScriptModuleImport> Bind(std::string alias) const;
        void Close() noexcept;

    private:
        struct Impl;
        explicit ScriptModuleReferenceInstallation(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Extensions
