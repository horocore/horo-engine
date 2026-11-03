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
                                                                          std::string_view module, std::string_view importId,
                                                                          std::string alias, ScriptCapabilityProfile);
    [[nodiscard]] Result<ScriptModuleImport> BindHeadlessModuleScriptImport(HeadlessExtensionHost &, std::string_view extension,
                                                                            std::string_view module, std::string_view importId,
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
        /** @brief Owner safe-point after VM preparation: validates again, invalidates old metadata plans and revokes old admission.
         * @param next Actual fully prepared compatible context. @return Failure leaves the old context unchanged.
         * @note No VM/provider callback or wait occurs. The runtime owner then performs its nonthrowing owner-pointer swap.
         */
        [[nodiscard]] Result<void> CommitReplacement(const ScriptModuleContext &next);
        [[nodiscard]] Result<ScriptHandle> IssueHandle(std::string_view alias, std::string type);
        [[nodiscard]] Result<void> ValidateHandle(const ScriptHandle &) const;
        /** @brief Revokes VM delivery and canonical admission before requesting queued job cancellation; never waits. */
        void Close() noexcept;

    private:
        friend struct ScriptModuleContextAccess;
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
        /** @brief Prepare a compatible context/program, then swap at the same owner safe point; preparation failure retains active state.
         * Subsequent script execution errors are new-program results. */
        [[nodiscard]] virtual Result<void> ReplaceCompatible(std::shared_ptr<ScriptModuleContext>, std::string_view source) = 0;
        virtual void Close() noexcept = 0;
    };
}  // namespace Horo::Extensions
