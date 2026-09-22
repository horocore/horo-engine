#pragma once

/**
 * @file EditorEventTypes.h
 * @brief Stable event identities shared by the process-to-editor event boundary.
 */

#include <cstdint>
#include <string_view>

namespace Horo {
    /** @brief Event identities admitted by the host-owned editor event boundary. */
    enum class EditorEventKind : std::uint8_t {
        ProjectOpened,
        ProjectClosed,
        AssetImported,
        AssetReloaded,
        OperationStoreRevisionChanged,
        ConsoleLog,
        MetricsChanged,
        ProfilerCaptureState,
        McpToolInvocation,
        Count,
    };

    /**
     * @brief Reports whether an event identity belongs to the current bounded contract.
     * @param kind Candidate event identity.
     * @return True when `kind` is one of the explicitly admitted identities.
     */
    [[nodiscard]] constexpr bool IsKnownEditorEventKind(const EditorEventKind kind) noexcept {
        return kind < EditorEventKind::Count;
    }

    /**
     * @brief Returns the stable textual identity used in diagnostics and descriptor validation.
     * @param kind Event identity.
     * @return Canonical event name, or `"unknown"` for an invalid representation.
     */
    [[nodiscard]] constexpr std::string_view ToString(const EditorEventKind kind) noexcept {
        switch (kind) {
            case EditorEventKind::ProjectOpened:
                return "project_opened";
            case EditorEventKind::ProjectClosed:
                return "project_closed";
            case EditorEventKind::AssetImported:
                return "asset_imported";
            case EditorEventKind::AssetReloaded:
                return "asset_reloaded";
            case EditorEventKind::OperationStoreRevisionChanged:
                return "operation_store_revision_changed";
            case EditorEventKind::ConsoleLog:
                return "console_log";
            case EditorEventKind::MetricsChanged:
                return "metrics_changed";
            case EditorEventKind::ProfilerCaptureState:
                return "profiler_capture_state";
            case EditorEventKind::McpToolInvocation:
                return "mcp_tool_invocation";
            case EditorEventKind::Count:
                break;
        }
        return "unknown";
    }

    /** @brief Bounded profiler lifecycle state exposed without capture content or failure text. */
    enum class ProfilerCaptureState : std::uint8_t {
        Idle,
        Arming,
        Capturing,
        Stopping,
        Finalizing,
        Complete,
        Failed,
    };
}  // namespace Horo
