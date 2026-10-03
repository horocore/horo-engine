#pragma once

#include "Horo/Application/NavigationBakeDiagnostics.h"

namespace Horo::Application::NavigationBakeDetail {
    /** @brief Serializes one bounded checkpoint for the shared operation-history consumer. */
    [[nodiscard]] std::string EncodeDiagnostic(const NavigationBakeDiagnosticRecord &record, const NavigationBakeDiagnosticsConfig &config);
    /** @brief Rejects malformed, oversized, foreign-owner and unsupported checkpoints before recovery. */
    [[nodiscard]] std::optional<NavigationBakeDiagnosticRecord> DecodeDiagnostic(std::string_view bytes,
                                                                                 const NavigationBakeDiagnosticsConfig &config);
    /** @brief Supplies stable code/severity/remediation for a closed producer event. */
    void DescribeDiagnostic(NavigationBakeDiagnosticRecord &record);
}  // namespace Horo::Application::NavigationBakeDetail
