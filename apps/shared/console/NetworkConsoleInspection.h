#pragma once

/** @file NetworkConsoleInspection.h @brief Non-installed application console adapter over the existing network diagnostic query. */
#include "Horo/Application/NetworkDebugger.h"
#include "Horo/Debug/CommandExecutor.h"

namespace Horo::Application::Internal {
    /** @brief Stable optional engine namespace identity for read-only network diagnostic provenance. */
    inline constexpr Debug::DebugCommandId NetworkConsoleStatusId = 0xDB600100;
    /** @brief Returns inert Developer/NonShipping/AuditLogged owner-thread metadata; does not query/start networking.
     * @return Process-lifetime net.debug_status metadata, admitted only by an explicitly composing host.
     */
    [[nodiscard]] Debug::DebugCommandDescriptor NetworkConsoleStatusDescriptor() noexcept;
    /** @brief Reads and validates one bounded immutable publication through its existing query owner.
     * @param query Explicit application capability. @return Coherent evidence or typed malformed/query failure, without partial output.
     * Query exceptions are translated without retaining exception text; no producer or control method is called.
     */
    [[nodiscard]] Result<NetworkDebuggerProjection> ReadNetworkConsoleInspection(const INetworkDebuggerQuery &query);
    /** @brief Binds only an admitted canonical status descriptor to the actual application query owner.
     * @param registry Existing immutable command authority. @param query Explicit retained application query capability.
     * @return Owned read-only binding, empty when absent, or typed descriptor/capability failure without querying the service.
     * No producer, transport, session admission, control capability or remote endpoint is created or discovered.
     * Construct and execute on the common command owner; shared query lifetime covers accepted queued inspections.
     * The common executor must own an audit adapter; rejected audit admission precedes any network query or operation record.
     */
    [[nodiscard]] Result<std::vector<Debug::CommandBinding>> BindNetworkConsoleInspection(
        std::shared_ptr<const Debug::CommandRegistry> registry, std::shared_ptr<const INetworkDebuggerQuery> query);
}  // namespace Horo::Application::Internal
