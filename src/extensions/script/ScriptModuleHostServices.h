#pragma once
#include "ScriptModuleProgram.h"

namespace Horo::Extensions {
    struct ScriptModuleOperationPolicy final {
        std::vector<ExtensionCapabilityId> projectCapabilities, contextCapabilities;
        std::vector<ScriptCapabilityScopeLease> scopes;
        bool nativeConsent{}, scriptConsent{};
    };

    [[nodiscard]] std::vector<ExtensionCapabilityId> ScriptModuleRequestedCapabilities(const ScriptModuleProgram &);

    class ScriptModuleOperation final {
    public:
        ~ScriptModuleOperation();
        [[nodiscard]] Result<void> Poll();
        [[nodiscard]] std::optional<ScriptCallResult> ResultSnapshot() const;
        void Cancel() noexcept;

    private:
        friend class ScriptModuleHostServices;
        struct Impl;
        explicit ScriptModuleOperation(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };

    /** Product-owned operation factory over actual shared native authorities; no secondary manager, registry or VM scheduler. */
    class ScriptModuleHostServices final : public std::enable_shared_from_this<ScriptModuleHostServices> {
    public:
        static Result<std::shared_ptr<ScriptModuleHostServices>> Create(std::shared_ptr<NativeExtensionServiceHost>, const JobSystem &,
                                                                        ScriptCapabilityProfile);
        static Result<std::shared_ptr<ScriptModuleHostServices>> Create(std::shared_ptr<class ModuleScriptHostAuthority>, const JobSystem &,
                                                                        ScriptCapabilityProfile);
        /** Actual product cold Headless factory, valid only while the real host is Configuring. */
        static Result<std::shared_ptr<ScriptModuleHostServices>> CreateConfiguringHeadless(HeadlessExtensionHost &, const JobSystem &);
        ~ScriptModuleHostServices();
        /** Cold before headless native activation; editor can prepare its builtin on the owner before an explicit operation. */
        [[nodiscard]] Result<void> PrepareReference(bool requested);
        [[nodiscard]] Result<std::unique_ptr<ScriptModuleOperation>> Begin(std::shared_ptr<const ScriptModuleProgram>,
                                                                           ScriptModuleOperationPolicy,
                                                                           const CancellationToken &cancellation = {});
        [[nodiscard]] ScriptModuleBindingCacheStats CacheStats() const noexcept;

    private:
        friend class ScriptModuleOperation;
        struct Impl;
        explicit ScriptModuleHostServices(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Extensions
