#pragma once
/** @file GameplayAiScriptBridge.h
 * @brief Typed script invocation adapter onto the same native AI capability and lifecycle.
 */
#include "Horo/Extensions/ScriptCapabilityContext.h"
#include "Horo/Gameplay/GameplayAiContext.h"

#include <array>
#include <optional>

namespace Horo::Gameplay {
    /** @brief Canonical language-neutral AI API identity. */
    inline constexpr std::string_view GameplayAiScriptApiId = "horo.gameplay.ai";
    /** @brief Builds inert API metadata. @param moduleId Owning extension module. @param capability Explicit service grant.
     * @return Versioned language-neutral descriptor; no provider registration or runtime mutation occurs. */
    [[nodiscard]] Extensions::ScriptExportDescriptor MakeGameplayAiScriptDescriptor(std::string moduleId,
                                                                                    Extensions::ExtensionCapabilityId capability);

    /**
     * @brief Simulation-safe-point adapter with bounded opaque resource projections.
     * @details The existing invocation registry owns all script handle/context/provider generations. Records only
     * map those handles onto the existing native AI authority. ReapRevokedAtSafePoint cancels and releases work
     * when script imports disappear; hosts call it before retiring script contexts and on each AI safe point.
     * No VM object, runtime pointer, backend, callback, or authored task execution implementation crosses this API.
     */
    class GameplayAiScriptBridge final {
    public:
        /** @brief Composes an explicit native module scope. @param context Same immutable context passed to native gameplay. */
        explicit GameplayAiScriptBridge(std::shared_ptr<const GameplayAiContext> context);
        ~GameplayAiScriptBridge();
        GameplayAiScriptBridge(const GameplayAiScriptBridge &) = delete;
        GameplayAiScriptBridge &operator=(const GameplayAiScriptBridge &) = delete;
        /** @brief Admits and executes one bounded synchronous operation through the script ABI.
         * @param binding Host-admitted exact import. @param provider Existing provider registration. @param request Owned request values.
         * @return Registry polling handle or typed admission failure; operation errors use structured completion. */
        [[nodiscard]] Result<Extensions::ScriptInvocationHandle> InvokeAtSafePoint(
            const Extensions::ScriptCapabilityBinding &binding, const Extensions::ScriptInvocationProviderRegistration &provider,
            Extensions::ScriptInvocationRequest request);
        /** @brief Cancels revoked script-owned tasks and retires resource projections on the simulation owner safe point. */
        void ReapRevokedAtSafePoint();

    private:
        struct Resource final {
            Extensions::ScriptCapabilityBinding binding;
            Extensions::ScriptHandle script;
            AI::AgentHandle agent;
            AI::TaskHandle task;
        };

        /** @brief Resolves an exact registry-issued resource under the requesting binding. */
        [[nodiscard]] Result<Resource *> Find(const Extensions::ScriptCapabilityBinding &binding, const Extensions::ScriptValue &value);
        /** @brief Executes the typed operation after ABI admission and cancellation checking. */
        [[nodiscard]] Result<Extensions::ScriptCallResult> Execute(const Extensions::ScriptCapabilityBinding &binding,
                                                                   const Extensions::ScriptInvocationProviderRegistration &provider,
                                                                   const Extensions::ScriptInvocationRequest &request);
        /** @brief Releases terminal native execution and its existing-registry resource together. */
        [[nodiscard]] Result<Extensions::ScriptCallResult> ReleaseResource(const Extensions::ScriptCapabilityBinding &binding,
                                                                           Resource &record);
        /** @brief Resolves durable identity and publishes an existing-registry opaque agent resource. */
        [[nodiscard]] Result<Extensions::ScriptCallResult> ResolveAgent(const Extensions::ScriptCapabilityBinding &binding,
                                                                        const Extensions::ScriptInvocationProviderRegistration &provider,
                                                                        const Extensions::ScriptInvocationRequest &request);
        /** @brief Admits exact module-owned task work before publishing its opaque resource. */
        [[nodiscard]] Result<Extensions::ScriptCallResult> StartTask(Resource &record, const Extensions::ScriptCapabilityBinding &binding,
                                                                     const Extensions::ScriptInvocationProviderRegistration &provider,
                                                                     const Extensions::ScriptInvocationRequest &request);
        /** @brief Projects one authorized native value into the typed script algebra. */
        [[nodiscard]] Result<Extensions::ScriptCallResult> ReadBlackboard(Resource &record,
                                                                          const Extensions::ScriptInvocationRequest &request);
        /** @brief Stages and atomically commits all script writes through the existing blackboard owner. */
        [[nodiscard]] Result<Extensions::ScriptCallResult> WriteBlackboard(Resource &record,
                                                                           const Extensions::ScriptInvocationRequest &request);
        /** @brief Retires a resource whose admission result could not be published, without invoking module code. */
        void RollbackResource(Resource &record) noexcept;
        Resource *pendingPublication_{};
        bool invoking_{};
        const std::thread::id owner_{std::this_thread::get_id()};
        std::shared_ptr<const GameplayAiContext> context_;
        std::array<std::optional<Resource>, AI::MaximumAiCapabilityTasks * 2> resources_{};
    };
}  // namespace Horo::Gameplay
