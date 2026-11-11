#pragma once

/** @file ConsoleTelemetryBindings.h @brief Explicit console adapters over the existing process Telemetry authority. */
#include "Horo/Debug/CommandExecutor.h"

namespace Horo::Debug {
    /** @brief Creates an audit adapter without starting, configuring or querying process telemetry.
     * @return Owned adapter that fails closed if the existing telemetry runtime rejects an event.
     * @note Success means admission to the existing asynchronous queue, not durable sink delivery.
     * Only prepared redacted text is accepted by ICommandAudit; ambient logging context is excluded.
     */
    [[nodiscard]] std::shared_ptr<ICommandAudit> CreateConsoleTelemetryAudit();

    /** @brief Binds admitted sys.metrics metadata to the existing privacy-safe Telemetry diagnostic snapshot.
     * @param registry Immutable inventory for the compiled product profile.
     * @return Owned read-only bindings, or typed composition failure. No telemetry query occurs during binding.
     * @note Reports process-local instrument IDs, generation and availability; does not invent metric names,
     * history or capture data. Shipping compiles an empty contribution instead of this diagnostic handler.
     */
    [[nodiscard]] Result<std::vector<CommandBinding>> BindConsoleTelemetryCommands(std::shared_ptr<const CommandRegistry> registry);
}  // namespace Horo::Debug
