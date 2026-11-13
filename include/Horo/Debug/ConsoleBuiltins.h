#pragma once

/** @file ConsoleBuiltins.h @brief Explicit host composition for safe, backend-independent console commands. */
#include "Horo/Debug/CommandExecutor.h"

namespace Horo::Debug {
    class ConsoleVariableSet;
    /** @brief Creates an audit adapter without starting, configuring or querying process telemetry.
     * @return Owned adapter failing closed when the existing telemetry runtime rejects an event.
     * Success means admission to the existing asynchronous queue, not durable sink delivery.
     * Only prepared redacted text is accepted by ICommandAudit; ambient logging context is excluded.
     */
    [[nodiscard]] std::shared_ptr<ICommandAudit> CreateConsoleTelemetryAudit();
    /** @brief Stable engine-owned core command identities; projects must not reuse these IDs. */
    inline constexpr DebugCommandId HelpCommandId = 0xDB600001;
    inline constexpr DebugCommandId FindCommandId = 0xDB600002;
    inline constexpr DebugCommandId VersionCommandId = 0xDB600003;
    inline constexpr DebugCommandId ClearCommandId = 0xDB600004;
    /** @brief Stable Developer command identity for the existing process Telemetry availability query. */
    inline constexpr DebugCommandId MetricsCommandId = 0xDB600007;
    /** @brief Stable DevelopmentOnly read-only Scene command identities. */
    inline constexpr DebugCommandId SceneTreeCommandId = 0xDB600009;
    inline constexpr DebugCommandId InspectCommandId = 0xDB60000A;

    /** @brief Finite inspection work; one command cannot scan an unbounded sparse Scene on the owner frame. */
    struct ConsoleSceneInspectionLimits final {
        std::size_t maximumVisitedSlots{4096};
        std::size_t maximumReportedEntities{256};
    };

    /** @brief Returns inert DevelopmentOnly Scene inspection metadata, without acquiring any scene.
     * @return Process-lifetime descriptors for hierarchy and generation-checked entity inspection.
     */
    [[nodiscard]] std::span<const DebugCommandDescriptor> ConsoleSceneDescriptors() noexcept;
    /** @brief Returns inert Developer/NonShipping owner-thread metric-availability metadata.
     * @return Borrowed process-lifetime text only; queries/starts no telemetry and binds no handler.
     * The separately linked metadata object is referenced only by compositions admitting this command.
     */
    [[nodiscard]] DebugCommandDescriptor ConsoleMetricDescriptor() noexcept;
    /** @brief Returns inert Public/ShippingAllowlist metadata for help, find, version and clear.
     * @return Process-lifetime metadata only; neither registers handlers nor starts a service.
     */
    [[nodiscard]] std::span<const DebugCommandDescriptor> CoreConsoleDescriptors() noexcept;
    /** @brief Binds backend-independent commands to the exact registry and visible history authority.
     * @param registry Composed immutable inventory. @param history Existing visible command/result history.
     * @param version Privacy-reviewed build/game/platform/profile identity without source paths.
     * @param variables Optional existing variable inventory; help/find use the same principal gates.
     * @return Owned bindings for present core commands, with no handler side effects during construction.
     * Clear is owner-thread scheduled because visible history belongs to its presentation owner.
     */
    [[nodiscard]] Result<std::vector<CommandBinding>> BindCoreConsoleCommands(std::shared_ptr<const CommandRegistry> registry,
                                                                              std::shared_ptr<ConsoleHistory> history, std::string version,
                                                                              std::shared_ptr<const ConsoleVariableSet> variables = {});
}  // namespace Horo::Debug
