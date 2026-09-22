#pragma once

/**
 * @file EditorDataBus.h
 * @brief Session-scoped editor notifications and sanitized process projections.
 */

#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/EditorEventTypes.h"
#include "Horo/Foundation/OperationStore.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace Horo::Editor {
    /** @brief Sanitized editor projection of a project-open notification. */
    struct EditorProjectOpenedEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.editor.project_opened.v1";

        std::uint64_t revision{};
    };

    /** @brief Sanitized editor projection of a project-close notification. */
    struct EditorProjectClosedEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.editor.project_closed.v1";

        std::uint64_t revision{};
    };

    /** @brief Sanitized editor projection of an imported asset identity. */
    struct EditorAssetImportedEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.editor.asset_imported.v1";

        std::uint64_t revision{};
        std::string assetId;
    };

    /** @brief Sanitized editor projection of a reloaded asset identity. */
    struct EditorAssetReloadedEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.editor.asset_reloaded.v1";

        std::uint64_t revision{};
        std::string assetId;
    };

    /** @brief Sanitized editor projection of an operation-store revision. */
    struct EditorOperationStoreRevisionChangedEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.editor.operation_store_revision_changed.v1";

        std::uint64_t revision{};
        std::optional<OperationId> changedOperation;
        OperationState state{OperationState::Queued};
    };

    /** @brief Bounded invalidation hint for newly appended console records. */
    struct EditorConsoleLogEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.editor.console_log.v1";

        std::uint64_t revision{};
        std::size_t appendedCount{};
    };

    /** @brief Bounded invalidation hint for changed metric groups. */
    struct EditorMetricsChangedEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.editor.metrics_changed.v1";

        std::uint64_t revision{};
        std::uint64_t changedGroups{};
    };

    /** @brief Sanitized profiler lifecycle state without capture data or failure text. */
    struct EditorProfilerCaptureStateEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.editor.profiler_capture_state.v1";

        std::uint64_t captureId{};
        ProfilerCaptureState state{ProfilerCaptureState::Idle};
    };

    /** @brief Sanitized MCP operation state without tool names, arguments, or request content. */
    struct EditorMcpToolInvocationEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.editor.mcp_tool_invocation.v1";

        OperationState state{OperationState::Queued};
        std::uint64_t historyRevision{};
    };

    // Short names are intentionally scoped to Editor so host code can use the
    // contract names from the architecture document without exposing process payloads.
    using ProjectOpenedEvent = EditorProjectOpenedEvent;
    using ProjectClosedEvent = EditorProjectClosedEvent;
    using AssetImportedEvent = EditorAssetImportedEvent;
    using AssetReloadedEvent = EditorAssetReloadedEvent;
    using OperationStoreRevisionChangedEvent = EditorOperationStoreRevisionChangedEvent;
    using ConsoleLogEvent = EditorConsoleLogEvent;
    using MetricsChangedEvent = EditorMetricsChangedEvent;
    using ProfilerCaptureStateEvent = EditorProfilerCaptureStateEvent;
    using McpToolInvocationEvent = EditorMcpToolInvocationEvent;

    /** @brief Session-scoped, editor-thread notification surface. */
    class EditorDataBus {
    public:
        EditorDataBus() = default;

        template <typename EventT, typename Handler> Subscription Subscribe(Handler &&handler) {
            AssertOwnerThread();
            return m_bus.Subscribe<EventT>(std::forward<Handler>(handler));
        }

        template <typename EventT> void Publish(const EventT &event) {
            AssertOwnerThread();
            m_bus.Publish(event);
        }

        void Clear() {
            AssertOwnerThread();
            m_bus.Clear();
        }

    private:
        void AssertOwnerThread() const noexcept;
        std::thread::id m_ownerThread = std::this_thread::get_id();
        EngineDataBus m_bus{EngineDataBusConfig{.logCategory = "editor.data_bus"}};
    };
}  // namespace Horo::Editor
