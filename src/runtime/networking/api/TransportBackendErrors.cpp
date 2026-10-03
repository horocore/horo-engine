#include "Horo/Network/NetworkErrors.h"

namespace Horo::Network::NetworkErrors {
    namespace {
        const ErrorDomainId Domain{"horo.network"};
    }

    const ErrorCodeDescriptor TransportBackendInvalid{Domain,
                                                      ErrorCode{"network.transport.backend_invalid"},
                                                      ErrorSeverity::Error,
                                                      "Transport backend composition input is invalid.",
                                                      "Use a canonical backend ID, coherent capabilities, and a non-null factory.",
                                                      false,
                                                      true};
    const ErrorCodeDescriptor TransportBackendUnavailable{Domain,
                                                          ErrorCode{"network.transport.backend_unavailable"},
                                                          ErrorSeverity::Error,
                                                          "The requested transport backend is not installed.",
                                                          "Install and explicitly register it; no fallback is selected.",
                                                          false,
                                                          true};
    const ErrorCodeDescriptor TransportBackendUnsupported{Domain,
                                                          ErrorCode{"network.transport.backend_unsupported"},
                                                          ErrorSeverity::Error,
                                                          "The installed transport backend is not supported by this host.",
                                                          "Choose a host-supported backend or change the product composition.",
                                                          false,
                                                          true};
    const ErrorCodeDescriptor TransportBackendNotConfigured{Domain,
                                                            ErrorCode{"network.transport.backend_not_configured"},
                                                            ErrorSeverity::Error,
                                                            "The installed transport backend is not configured.",
                                                            "Supply a complete admitted host configuration before selection.",
                                                            false,
                                                            true};
    const ErrorCodeDescriptor TransportBackendConflict{Domain,
                                                       ErrorCode{"network.transport.backend_conflict"},
                                                       ErrorSeverity::Error,
                                                       "A backend identity or selection conflicts with the current owner.",
                                                       "Register each ID once and select one backend per composition lifetime.",
                                                       false,
                                                       true};
    const ErrorCodeDescriptor TransportBackendCapacityExceeded{Domain,
                                                               ErrorCode{"network.transport.backend_capacity_exceeded"},
                                                               ErrorSeverity::Error,
                                                               "The transport backend catalog is full.",
                                                               "Reduce registrations to the finite host limit.",
                                                               false,
                                                               true};
    const ErrorCodeDescriptor TransportBackendFactoryFailed{Domain,
                                                            ErrorCode{"network.transport.backend_factory_failed"},
                                                            ErrorSeverity::Error,
                                                            "The selected backend factory produced no instance.",
                                                            "Repair the selected factory; selection remains inactive.",
                                                            false,
                                                            false};
    const ErrorCodeDescriptor TransportBackendCancelled{Domain,
                                                        ErrorCode{"network.transport.backend_cancelled"},
                                                        ErrorSeverity::Warning,
                                                        "Transport backend composition was cancelled.",
                                                        "Do not retry in this composition lifetime.",
                                                        false,
                                                        false};
    const ErrorCodeDescriptor TransportBackendShuttingDown{Domain,
                                                           ErrorCode{"network.transport.backend_shutting_down"},
                                                           ErrorSeverity::Info,
                                                           "Transport backend composition is closed.",
                                                           "Create a new composition owner before selecting a backend.",
                                                           false,
                                                           false};
}  // namespace Horo::Network::NetworkErrors
