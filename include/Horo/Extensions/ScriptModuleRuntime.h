#pragma once
/** @file ScriptModuleRuntime.h @brief Two-language module programs over the existing script capability/invocation authorities. */
#include "Horo/Extensions/ScriptRuntimeBackendBinding.h"
#include "Horo/Foundation/JobSystem.h"

#include <chrono>
#include <optional>

namespace Horo::Extensions {
    enum class ScriptModuleRuntimeKind : std::uint8_t {
        Lua54,
        JavaScriptNg
    };

    struct ScriptModuleRuntimeIdentity final {
        ScriptModuleRuntimeKind kind{ScriptModuleRuntimeKind::Lua54};
        std::string version;
        std::string targetProfile;
        Sha256Digest implementation;
        bool operator==(const ScriptModuleRuntimeIdentity &) const noexcept = default;
    };

    struct ScriptModuleRuntimeLimits final {
        std::size_t maximumSourceBytes{64U * 1024U};
        std::size_t maximumMemoryBytes{4U * 1024U * 1024U};
        std::size_t maximumStackBytes{256U * 1024U};
        std::size_t maximumPendingCalls{64};
        std::size_t maximumBridgeBytes{4U * 1024U *
                                       1024U}; /**< Bounded owned canonical payload/bridge metadata, separate from VM allocator. */
        std::size_t maximumBridgeValues{1024};
        std::size_t maximumEventsPerPoll{64};
        std::size_t maximumVmJobsPerPoll{64};
        std::chrono::milliseconds executionDeadline{std::chrono::seconds{5}};
        std::uint32_t luaInstructionBudget{100'000};
        ScriptValueLimits values;
    };

    /** @brief Host-resolved local alias; construction retains the actual unforgeable module resolution. */
    class ScriptModuleImport final {
    public:
        [[nodiscard]] const std::string &Alias() const noexcept {
            return alias_;
        }

        [[nodiscard]] const ResolvedExtensionServiceImport &Resolution() const noexcept {
            return resolution_;
        }

        [[nodiscard]] const std::shared_ptr<const ScriptRuntimeBackendBinding> &Backend() const noexcept {
            return backend_;
        }

        [[nodiscard]] const std::optional<Error> &Unavailable() const noexcept {
            return unavailable_;
        }

    private:
        friend Result<ScriptModuleImport> BindScriptModuleImport(std::string, std::shared_ptr<const ScriptRuntimeBackendBinding>);
        friend Result<ScriptModuleImport> MakeUnavailableScriptModuleImport(std::string, const ResolvedExtensionServiceImport &);

        ScriptModuleImport(std::string alias, ResolvedExtensionServiceImport resolution,
                           std::shared_ptr<const ScriptRuntimeBackendBinding> backend, std::optional<Error> unavailable)
            : alias_(std::move(alias)), resolution_(std::move(resolution)), backend_(std::move(backend)),
              unavailable_(std::move(unavailable)) {}

        std::string alias_;
        ResolvedExtensionServiceImport resolution_;
        std::shared_ptr<const ScriptRuntimeBackendBinding> backend_;
        std::optional<Error> unavailable_;
    };

    [[nodiscard]] Result<ScriptModuleImport> BindScriptModuleImport(std::string alias, std::shared_ptr<const ScriptRuntimeBackendBinding>);
    /** @brief Creates an unavailable projection only from an actual non-required unresolved module import. */
    [[nodiscard]] Result<ScriptModuleImport> MakeUnavailableScriptModuleImport(std::string alias,
                                                                               const ResolvedExtensionServiceImport &resolved);
    class NativeExtensionServiceHost;
    class HeadlessExtensionHost;
    /** @brief Owner-only projection of the actual already-activated native module import; no new registration/admission. */
    [[nodiscard]] Result<ScriptModuleImport> BindNativeModuleScriptImport(NativeExtensionServiceHost &, std::string_view extension,
                                                                          std::string_view moduleId, std::string_view importId,
                                                                          std::string alias, ScriptCapabilityProfile);
    [[nodiscard]] Result<ScriptModuleImport> BindHeadlessModuleScriptImport(HeadlessExtensionHost &, std::string_view extension,
                                                                            std::string_view moduleId, std::string_view importId,
                                                                            std::string alias, ScriptCapabilityProfile);

    /** @brief Canonical immutable plan key; never contains live VM closures or permission authority. */
    struct ScriptModuleBindingPlan final {
        Sha256Digest key;
        ScriptModuleRuntimeIdentity runtime;
        std::weak_ptr<const ScriptExportDescriptorSnapshot> descriptors;
        Sha256Digest descriptorFingerprint;
        std::string apiId;
        std::vector<std::string> namespaceSegments;
        std::vector<std::size_t> functionIndices, constantIndices;
        std::size_t retainedBytes{};
    };

    struct ScriptModuleBindingCacheStats final {
        std::size_t entries{}, retainedBytes{};
        std::uint64_t hits{}, misses{};
    };

    class ScriptModuleBindingCache final {
    public:
        explicit ScriptModuleBindingCache(std::size_t maximumEntries = 64, std::size_t maximumBytes = 16U * 1024U * 1024U);
        ~ScriptModuleBindingCache();
        ScriptModuleBindingCache(const ScriptModuleBindingCache &) = delete;
        ScriptModuleBindingCache &operator=(const ScriptModuleBindingCache &) = delete;
        /** @brief Produces the validated recipe; finite cache capacity controls residency, not logical API availability.
         * @return Shared resident recipe, or an equivalent caller-owned uncached recipe while resident plans remain pinned.
         * @note Stats().retainedBytes counts resident plans only. Uncached recipes remain owned/bounded by their caller lifecycle.
         * Existing pinned recipes are never evicted or mutated to admit a candidate.
         */
        [[nodiscard]] Result<std::shared_ptr<const ScriptModuleBindingPlan>> Acquire(const ScriptModuleRuntimeIdentity &,
                                                                                     const ScriptRuntimeBackendBinding &,
                                                                                     const ScriptCapabilityContextDescriptor &);
        [[nodiscard]] ScriptModuleBindingCacheStats Stats() const noexcept;
        /** @brief Owner safe-point invalidation; retained old plans stay immutable and confer no liveness. */
        void Invalidate(const Sha256Digest &descriptor);
        void InvalidateKey(const Sha256Digest &key);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    /** Actual source activation issued by trusted composition, never VM data or a permission request. */
    struct ScriptModuleExecutionOwner final {
        ExtensionCapabilityHandle capability;
        BackendServiceCodeLease code;

        ScriptModuleExecutionOwner(ExtensionCapabilityHandle actual, BackendServiceCodeLease executable)
            : capability(std::move(actual)), code(std::move(executable)) {}
    };

    /** @brief Actual API update classification; unknown native image candidates are never called compatible by version text alone. */
    enum class ScriptModuleUpdateKind : std::uint8_t {
        Unchanged,
        Compatible,
        Breaking,
        Unavailable,
        RestartRequired
    };

    /** @brief Alias-sorted actual descriptor fingerprint; absence identifies an unresolved optional import without inventing a digest. */
    struct ScriptModuleApiFingerprint final {
        std::string alias;                       /**< Actual admitted alias, ordered lexicographically in reports. */
        std::optional<Sha256Digest> fingerprint; /**< Canonical descriptor digest, empty only for unavailable imports. */
        bool operator==(const ScriptModuleApiFingerprint &) const noexcept = default;
    };

    /** @brief Owned nonadmitting recovery evidence; none of these fields grants execution or destroys active state. */
    struct ScriptModuleUpdateReport final {
        ScriptModuleUpdateKind kind{ScriptModuleUpdateKind::Unavailable};
        std::vector<ScriptModuleApiFingerprint> previousFingerprints, candidateFingerprints;
        std::optional<Error> diagnostic; /**< Owned registered diagnostic; never a replacement authorization. */
    };

    /** @brief Sealed engine-prepared namespace/context publication for one actual VM; no caller callbacks cross the commit boundary.
     * @details Destruction, validation and commit belong to the VM owner lane. A preparation pins the actual VM state, not its caller.
     * Keep preparation within the explicit owner safe point; never destroy it on a worker.
     */
    class ScriptModulePreparedRebind final {
    public:
        ~ScriptModulePreparedRebind();
        ScriptModulePreparedRebind(const ScriptModulePreparedRebind &) = delete;
        ScriptModulePreparedRebind &operator=(const ScriptModulePreparedRebind &) = delete;
        /** @brief Validates the same actual owner/generation and live candidate immediately before publication.
         * @return Failure retains the original VM state, namespaces, context and source execution position. */
        [[nodiscard]] Result<void> Validate();
        /** @brief Publishes only an already-validated engine preparation without allocation, provider/user callbacks or waits.
         * @return True once on the actual owner safe point; false for consumed/unvalidated or wrong-owner use.
         * @pre No intervening owner mutation since Validate. Concurrent Close remains supported and sticky.
         */
        [[nodiscard]] bool Commit() noexcept;

    private:
        friend struct ScriptModuleRebindAccess;
        struct Impl;
        explicit ScriptModulePreparedRebind(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };

    /** @brief Owner-lane actual context; workers retain only backend/controller/owned values. */
    class ScriptModuleContext final {
    public:
        /** @pre jobs and cache belong to the real host and outlive all accepted work/context operations. */
        [[nodiscard]] static Result<std::shared_ptr<ScriptModuleContext>> Create(
            std::shared_ptr<ScriptInvocationRegistry>, const JobSystem &jobs, ScriptModuleBindingCache &cache, ScriptModuleRuntimeIdentity,
            ScriptCapabilityContextDescriptor, std::vector<ScriptModuleImport>, ScriptModuleRuntimeLimits = {}, CancellationToken = {},
            std::optional<ScriptModuleExecutionOwner> executionOwner = {});
        ~ScriptModuleContext();
        [[nodiscard]] std::span<const ScriptModuleImport> Imports() const noexcept;
        [[nodiscard]] const ScriptModuleRuntimeIdentity &Runtime() const noexcept;
        [[nodiscard]] const ScriptModuleRuntimeLimits &Limits() const noexcept;
        [[nodiscard]] bool IsUsable() const noexcept;
        [[nodiscard]] Result<ScriptInvocationHandle> Invoke(std::string_view alias, std::string_view function,
                                                            std::vector<ScriptValue> arguments);
        [[nodiscard]] Result<std::vector<ScriptInvocationEvent>> Poll();
        [[nodiscard]] Result<std::shared_ptr<const ScriptModuleBindingPlan>> BindingPlan(std::string_view alias) const;
        [[nodiscard]] Result<void> ValidateReplacement(const ScriptModuleContext &) const;
        /** @brief Derives a candidate using the SAME actual registry, policy, runtime, scopes, source owner and resource bounds.
         * @param imports Fresh host-issued exact imports; no provider is discovered or registered here.
         * @return Fully admitted compatible candidate, or typed failure retaining this context.
         */
        [[nodiscard]] Result<std::shared_ptr<ScriptModuleContext>> PrepareReplacement(std::vector<ScriptModuleImport> imports) const;
        /** @brief Compares actual canonical fingerprints through existing descriptor compatibility validation.
         * @param next Actual candidate context. @return Owned classification/diagnostic without changing either context.
         */
        [[nodiscard]] Result<ScriptModuleUpdateReport> InspectReplacement(const ScriptModuleContext &next) const;
        /** @brief Stops NEW invocation and handle admission without revoking accepted work or its results.
         * @return True on the actual owner lane (also when already sealed); false for wrong-lane use.
         * @details Owner safe-point only. Explicit Close still cancels all work normally. */
        [[nodiscard]] bool SealNewCalls() noexcept;
        /** @brief Pumps only accepted predecessor work/events without VM delivery, blocking or new admission.
         * @return True after all real job callbacks completed; false while work remains; typed affinity/protocol error otherwise.
         * @pre SealNewCalls was called by the owner. The actual predecessor provider/source leases stay retained. */
        [[nodiscard]] Result<bool> PollRetirement();

        /** @brief Owner safe-point after VM preparation: validates again, invalidates old metadata plans and seals new admission.
         * @param next Actual fully prepared compatible context. @return Failure leaves the old context unchanged.
         * @note Accepted work is not cancelled by this seal. No VM/provider callback or wait occurs; the runtime publishes its prepared
         * namespace/context swap at the same owner safe point.
         */
        [[nodiscard]] Result<void> CommitReplacement(const ScriptModuleContext &next);
        [[nodiscard]] Result<ScriptHandle> IssueHandle(std::string_view alias, std::string type);
        [[nodiscard]] Result<void> ValidateHandle(const ScriptHandle &) const;
        /** @brief Revokes VM delivery and canonical admission before requesting queued job cancellation; never waits. */
        void Close() noexcept;

    private:
        friend struct ScriptModuleContextAccess;
        friend struct ScriptModuleRebindAccess;
        bool CommitPreparedReplacement(const ScriptModuleContext &) noexcept;
        struct Impl;
        explicit ScriptModuleContext(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };

    /** @brief VM-neutral module owner; all methods except Close are owner-lane operations. */
    class IScriptModuleRuntime {
    public:
        virtual ~IScriptModuleRuntime() = default;
        [[nodiscard]] virtual Result<void> Start(std::string_view source) = 0;
        [[nodiscard]] virtual Result<void> Poll() = 0;
        [[nodiscard]] virtual std::optional<ScriptCallResult> ResultSnapshot() const = 0;
        /** @brief Prepares compatible namespaces in the SAME VM without rerunning source or changing user locals/closures.
         * @param next Actual same-registry/scopes/source candidate. @return Sealed publication or typed last-good failure.
         * @details At most one preparation and one draining predecessor; accepted pending operations retain their actual origin.
         */
        [[nodiscard]] virtual Result<std::unique_ptr<ScriptModulePreparedRebind>> PrepareRebind(
            std::shared_ptr<ScriptModuleContext> next) = 0;
        /** @brief Pumps only real predecessor retirement; never executes source, waits or admits another update.
         * @return True when there is no remaining predecessor, false while accepted work/delivery remains, or typed owner/reentrancy
         * failure. */
        [[nodiscard]] virtual Result<bool> PollRebindRetirement() = 0;
        /** @brief Convenience owner-safe-point update without replacing the running source or VM state.
         * @param next Fresh compatible actual context. @param source Original source, checked by digest; never re-evaluated.
         * @return Typed failure retains previous state; success refreshes bindings in the same VM.
         * @details A source edit requires explicit new-run lifecycle rather than this provider API update.
         */
        [[nodiscard]] virtual Result<void> ReplaceCompatible(std::shared_ptr<ScriptModuleContext> next, std::string_view source) = 0;
        virtual void Close() noexcept = 0;
    };
}  // namespace Horo::Extensions
