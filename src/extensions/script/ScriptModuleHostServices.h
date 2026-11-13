#pragma once
#include "ScriptModuleProgram.h"

namespace Horo::Extensions {
    class ScriptModuleReferenceInstallation;

    struct ScriptModuleOperationPolicy final {
        std::vector<ExtensionCapabilityId> projectCapabilities;
        std::vector<ExtensionCapabilityId> contextCapabilities;
        std::vector<ScriptCapabilityScopeLease> scopes;
        bool nativeConsent{};
        bool scriptConsent{};
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

    /** Owner-lane borrowed operation plus freshly admitted imports from this SAME host's actual registries.
     * The borrow must remain alive throughout synchronous Rebind; imports carry sealed provider evidence. */
    struct ScriptModuleOperationBindings final {
        ScriptModuleOperation *operation{};
        std::vector<ScriptModuleImport> imports;
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
        /** Explicit builtin additive-provider request, prepared through the generic all-operation rebind path.
         * No native library replacement is attempted. One active and one draining predecessor are retained. */
        [[nodiscard]] Result<std::vector<ScriptModuleUpdateReport>> RefreshReference(ScriptExportVersion);
        [[nodiscard]] Result<bool> PollProviderRetirement();
        /** Verify the actual signed candidate under current independent trust, but never stage/unload/replace a live native module.
         * A real live source owner must exist. Its prior provider stays admitted; unknown native API safety requires restart. */
        [[nodiscard]] Result<ScriptModuleUpdateReport> RequestNativeUpdate(const ScriptModuleProgram &);
        [[nodiscard]] ScriptModuleBindingCacheStats CacheStats() const noexcept;
        /** Prepare every namespace/descriptor candidate before committing any. Owner safe point only; no source rerun.
         * Failure retains every previous active binding. Accepted predecessor work remains deliverable in the same VM.
         * At most MaximumOperations entries; duplicate/foreign operations and a second draining predecessor fail closed. */
        [[nodiscard]] Result<std::vector<ScriptModuleUpdateReport>> Rebind(std::vector<ScriptModuleOperationBindings>);

    private:
        friend class ScriptModuleOperation;
        struct Impl;
        explicit ScriptModuleHostServices(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
        Result<std::vector<ScriptModuleUpdateReport>> RefreshReferenceOwned(ScriptExportVersion);
        Result<std::vector<ScriptModuleOperationBindings>> ReferenceUpdates(const ScriptModuleReferenceInstallation &);
        Result<std::vector<ScriptModuleUpdateReport>> RebindOwned(std::vector<ScriptModuleOperationBindings>);
        Result<std::vector<ScriptModuleUpdateReport>> CommitOwned(std::vector<struct ScriptModuleHostRebindCandidate> &);
    };
}  // namespace Horo::Extensions
