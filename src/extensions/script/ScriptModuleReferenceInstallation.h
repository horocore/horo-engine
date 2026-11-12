#pragma once
#include "Horo/Extensions/ScriptModuleRuntime.h"

namespace Horo::Extensions {
    /** Explicit builtin provider installed in the actual host authorities; not a native package or VM authority. */
    class ScriptModuleReferenceInstallation final {
        struct Impl;

        class ConstructionKey final {
            ConstructionKey() = default;
            friend class ScriptModuleReferenceInstallation;
        };

    public:
        /** @brief Retains prepared state without registering capabilities or services.
         * @param key Factory-only token, issued after host and schema admission.
         * @param value Owned prepared state transferred to this installation. */
        explicit ScriptModuleReferenceInstallation(ConstructionKey key, std::unique_ptr<Impl> value);
        static Result<std::unique_ptr<ScriptModuleReferenceInstallation>> Create(std::shared_ptr<class ModuleScriptHostAuthority>,
                                                                                 ScriptCapabilityProfile,
                                                                                 ScriptExportVersion version = {1, 0, 0});
        ~ScriptModuleReferenceInstallation();
        [[nodiscard]] Result<ScriptModuleImport> Bind(std::string alias) const;
        [[nodiscard]] ScriptExportVersion Version() const noexcept;
        void Close() noexcept;

    private:
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Extensions
