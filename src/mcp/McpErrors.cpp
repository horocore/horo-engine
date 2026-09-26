#include "Horo/Mcp/McpErrors.h"

namespace Horo::Mcp::McpErrors {
    const ErrorCodeDescriptor ConfigurationInvalid{ErrorDomainId{"horo.mcp"}, ErrorCode{"configuration_invalid"}, ErrorSeverity::Error,
                                                   "MCP session configuration is invalid.",
                                                   "Declare finite nonzero limits and a controller."};
    const ErrorCodeDescriptor AdmissionInvalid{ErrorDomainId{"horo.mcp"}, ErrorCode{"admission_invalid"}, ErrorSeverity::Error,
                                               "MCP caller admission is invalid.", "Approve a named local client with bounded authority."};
    const ErrorCodeDescriptor SessionCapacityExceeded{ErrorDomainId{"horo.mcp"}, ErrorCode{"session_capacity_exceeded"},
                                                      ErrorSeverity::Warning, "The MCP session limit was reached.",
                                                      "Close an idle session before retrying."};
    const ErrorCodeDescriptor SessionUnavailable{ErrorDomainId{"horo.mcp"}, ErrorCode{"session_unavailable"}, ErrorSeverity::Warning,
                                                 "The MCP session is closed or stale.", "Start a new approved session."};
    const ErrorCodeDescriptor ShuttingDown{ErrorDomainId{"horo.mcp"}, ErrorCode{"shutting_down"}, ErrorSeverity::Warning,
                                           "The MCP host is shutting down.", "Retry after host startup."};
    const ErrorCodeDescriptor RequestInvalid{ErrorDomainId{"horo.mcp"}, ErrorCode{"request_invalid"}, ErrorSeverity::Error,
                                             "The MCP request is malformed or already active.", "Use a unique bounded request ID."};
    const ErrorCodeDescriptor RequestCapacityExceeded{ErrorDomainId{"horo.mcp"}, ErrorCode{"request_capacity_exceeded"},
                                                      ErrorSeverity::Warning, "The session request limit was reached.",
                                                      "Wait for an active request to finish."};
    const ErrorCodeDescriptor InputCapacityExceeded{ErrorDomainId{"horo.mcp"}, ErrorCode{"input_capacity_exceeded"}, ErrorSeverity::Error,
                                                    "The MCP input exceeds a declared limit.", "Reduce the request size or complexity."};
    const ErrorCodeDescriptor ResultCapacityExceeded{ErrorDomainId{"horo.mcp"}, ErrorCode{"result_capacity_exceeded"}, ErrorSeverity::Error,
                                                     "The MCP result exceeds a declared limit.", "Request a smaller bounded result."};
    const ErrorCodeDescriptor RequestCancelled{ErrorDomainId{"horo.mcp"}, ErrorCode{"request_cancelled"}, ErrorSeverity::Warning,
                                               "The MCP request was cancelled.", "Retry only if the operation is safe to repeat."};
    const ErrorCodeDescriptor RequestTimedOut{ErrorDomainId{"horo.mcp"}, ErrorCode{"request_timed_out"}, ErrorSeverity::Warning,
                                              "The MCP request deadline elapsed.", "Retry with a smaller bounded operation."};
    const ErrorCodeDescriptor ControllerFailed{ErrorDomainId{"horo.mcp"}, ErrorCode{"controller_failed"}, ErrorSeverity::Error,
                                               "The MCP controller failed unexpectedly.", "Inspect host diagnostics."};
    const ErrorCodeDescriptor DrainTimedOut{ErrorDomainId{"horo.mcp"}, ErrorCode{"drain_timed_out"}, ErrorSeverity::Error,
                                            "MCP callbacks did not drain before shutdown's deadline.",
                                            "Keep application leases alive until the callbacks finish."};
}  // namespace Horo::Mcp::McpErrors
