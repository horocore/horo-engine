#pragma once

/**
 * @file ProcessEvents.h
 * @brief Typed process-scoped events consumed by the editor event bridge.
 */

#include "Horo/Foundation/EditorEventTypes.h"
#include "Horo/Foundation/OperationStore.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Horo {
    /** @brief Process notification that a project became active. */
    struct ProjectOpenedEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.process.project_opened.v1";

        std::uint64_t revision{};
        std::string projectId;
        std::filesystem::path projectRoot;
    };

    /** @brief Process notification that the active project was closed. */
    struct ProjectClosedEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.process.project_closed.v1";

        std::uint64_t revision{};
        std::string projectId;
    };

    /** @brief Process notification that an asset import completed. */
    struct AssetImportedEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.process.asset_imported.v1";

        std::uint64_t revision{};
        std::string assetId;
        std::filesystem::path sourcePath;
    };

    /** @brief Process notification that an asset was reloaded. */
    struct AssetReloadedEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.process.asset_reloaded.v1";

        std::uint64_t revision{};
        std::string assetId;
        std::filesystem::path sourcePath;
    };

    /** @brief Process notification that the bounded operation projection advanced. */
    struct OperationStoreRevisionChangedEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.process.operation_store_revision_changed.v1";

        std::uint64_t revision{};
        std::optional<OperationId> changedOperation;
        OperationState state{OperationState::Queued};
    };

    /** @brief Process notification that new console records are available in the log store. */
    struct ConsoleLogEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.process.console_log.v1";

        std::uint64_t revision{};
        std::size_t appendedCount{};
        std::string category;
        std::string message;
    };

    /** @brief Process notification that one or more metric groups changed. */
    struct MetricsChangedEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.process.metrics_changed.v1";

        std::uint64_t revision{};
        std::uint64_t changedGroups{};
    };

    /** @brief Process notification for profiler capture lifecycle only. */
    struct ProfilerCaptureStateEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.process.profiler_capture_state.v1";

        std::uint64_t captureId{};
        ProfilerCaptureState state{ProfilerCaptureState::Idle};
        std::string failureReason;
    };

    /** @brief Process notification for an MCP-backed operation without request content. */
    struct McpToolInvocationEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.process.mcp_tool_invocation.v1";

        std::uint64_t requestId{};
        std::string toolName;
        OperationId operationId{};
        OperationState state{OperationState::Queued};
        std::uint64_t historyRevision{};
        std::string arguments;
    };
}  // namespace Horo
