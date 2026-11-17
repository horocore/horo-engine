#pragma once

/** @file NativeExtensionServiceHost.h
 * @brief Composition-owned ABI adapter into the canonical backend and script authorities.
 */
#include "Horo/Extensions/BackendServiceRegistry.h"
#include "Horo/Extensions/ScriptCapabilityContext.h"

#include <span>
#include <thread>

namespace Horo::Extensions {
    struct NativeExtensionServiceState;
    struct NativeExtensionServiceHostAccess;
    class NativeExtensionScriptSession;

    /** @brief Owner-lane native service composition retaining the actual canonical registry owners. */
    class NativeExtensionServiceHost final {
    public:
        /** @brief Creates an adapter over existing authorities, with explicit immutable permission approval.
         * @param capabilities Shared actual application provider authority.
         * @param services Shared actual canonical service authority.
         * @param invocations Shared canonical invocation authority.
         * @param policy Actual host policy; no package, path or example implicitly grants permissions.
         */
        NativeExtensionServiceHost(std::shared_ptr<ApplicationCapabilityRegistry> capabilities,
                                   std::shared_ptr<BackendServiceRegistry> services, std::shared_ptr<ScriptInvocationRegistry> invocations,
                                   ExtensionAdmissionPolicy policy);
        ~NativeExtensionServiceHost();
        NativeExtensionServiceHost(const NativeExtensionServiceHost &) = delete;
        NativeExtensionServiceHost &operator=(const NativeExtensionServiceHost &) = delete;

        /** @brief Atomically closes admission on any thread; owner lane also revokes module registrations.
         * @note Off-lane teardown uses actual manager/retirement owners for native finalization or restart retention;
         * it never traverses owner-only tables or proves native drain.
         */
        void BeginShutdown() noexcept;

        /** @brief Binds one actual importing module's declared API using explicit real script policy.
         * @param extension Exact active package.
         * @param moduleId Exact active importing module.
         * @param importId Exact manifest import local to that module.
         * @param policy Actual runtime trust/scopes/capability intersection; imports are host-derived.
         * @return Owner-lane session or typed missing/denied/stale/shape failure.
         */
        [[nodiscard]] Result<std::shared_ptr<NativeExtensionScriptSession>> BindScript(std::string_view extension,
                                                                                       std::string_view moduleId, std::string_view importId,
                                                                                       ScriptCapabilityContextDescriptor policy,
                                                                                       const CancellationToken &cancellation = {});

    private:
        friend struct NativeExtensionServiceHostAccess;
        std::shared_ptr<NativeExtensionServiceState> state_;
    };

    /** @brief Exact script binding; owns context and schema evidence, never a VM or module authoring authority. */
    class NativeExtensionScriptSession final {
    public:
        ~NativeExtensionScriptSession();
        /** @brief Returns immutable validated functions for the runtime adapter. */
        [[nodiscard]] std::span<const ScriptExportFunctionDescriptor> Functions() const noexcept;
        /** @brief Synchronously invokes an admitted scalar method through the existing invocation lifecycle.
         * @param function Exact schema function. @param arguments Typed int64 values.
         * @param cancellation Actual caller cancellation. @return Result or attributed typed error.
         */
        [[nodiscard]] Result<std::int64_t> Invoke(std::string_view function, std::span<const std::int64_t> arguments,
                                                  const CancellationToken &cancellation = {});
        /** @brief Revokes delivery and every copied binding before native module retirement. */
        void Reset() noexcept;

    private:
        friend class NativeExtensionServiceHost;
        struct Impl;
        explicit NativeExtensionScriptSession(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Extensions
