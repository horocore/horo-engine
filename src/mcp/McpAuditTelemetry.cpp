#include "McpAuditTelemetry.h"

#include "Horo/Foundation/Telemetry/Telemetry.h"

namespace Horo::Mcp {
    /** @copydoc EmitMcpAudit */
    void EmitMcpAudit(const McpAuditEvent &event) noexcept {
        constexpr std::string_view subsystem = "mcp.audit";
        if (!Telemetry::Runtime::IsEventEnabled(subsystem, Log::Level::Info))
            return;
        try {
            static_cast<void>(
                Telemetry::Runtime::EmitRecord({.subsystem = std::string{subsystem},
                                                .context = Log::LogContextSnapshot::Isolated({}),
                                                .payload = Telemetry::LogRecord{.severity = Log::Level::Info,
                                                                                .category = std::string{subsystem},
                                                                                .message = std::string{event.event},
                                                                                .fields = {{"schema_version", std::uint64_t{1}},
                                                                                           {"principal_id", event.principal},
                                                                                           {"session_id", event.session.id},
                                                                                           {"session_generation", event.session.generation},
                                                                                           {"operation_id", event.operation},
                                                                                           {"approval_id", event.approval},
                                                                                           {"tool", std::string{event.tool}},
                                                                                           {"status", std::string{event.status}},
                                                                                           {"elapsed_ms", event.elapsed.count()}}}}));
        } catch (...) {  // Audit allocation or dispatcher failure cannot affect command execution or owner teardown.
        }
    }
}  // namespace Horo::Mcp
