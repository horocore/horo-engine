#pragma once

/**
 * @file EditorEngineEventBridge.h
 * @brief Explicit process-to-editor event projection owned by an editor session.
 */

#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Foundation/DataBus.h"

#include <cstdint>
#include <memory>

namespace Horo::Editor {
    /** @brief Counters for accepted, rejected, and incorrectly threaded bridge notifications. */
    struct EditorEngineEventBridgeStats final {
        std::uint64_t forwarded{};
        std::uint64_t filtered{};
        std::uint64_t threadDrops{};
    };

    /**
     * @brief Projects the fixed process-event allowlist into a session-owned EditorDataBus.
     *
     * The bridge is deliberately the only process-to-editor subscription point. It drops
     * sensitive process fields while constructing editor payloads and never exposes either
     * process bus to an extension surface.
     */
    class EditorEngineEventBridge final {
    public:
        /**
         * @brief Creates a detached bridge for one editor session.
         * @param engineEvents Process-scoped source bus.
         * @param editorEvents Session-scoped destination bus.
         */
        EditorEngineEventBridge(EngineDataBus &engineEvents, EditorDataBus &editorEvents);
        /** @brief Detaches all process subscriptions before releasing the session references. */
        ~EditorEngineEventBridge();
        EditorEngineEventBridge(const EditorEngineEventBridge &) = delete;
        EditorEngineEventBridge &operator=(const EditorEngineEventBridge &) = delete;
        /** @brief Bridge identity is tied to one immutable process/editor bus pair. */
        EditorEngineEventBridge(EditorEngineEventBridge &&) = delete;
        /** @brief Bridge identity is tied to one immutable process/editor bus pair. */
        EditorEngineEventBridge &operator=(EditorEngineEventBridge &&) = delete;

        /**
         * @brief Attaches the fixed allowlist atomically.
         *
         * A subscription-limit failure leaves the bridge fully detached. Repeated calls while
         * attached are no-ops.
         */
        void Attach();

        /** @brief Revokes every process subscription before the destination session is torn down. */
        void Detach() noexcept;

        /** @return True when all allowlisted subscriptions are active. */
        [[nodiscard]] bool IsAttached() const noexcept;

        /** @return A snapshot of bridge filtering and forwarding counters. */
        [[nodiscard]] EditorEngineEventBridgeStats Stats() const noexcept;

    private:
        struct State;
        std::shared_ptr<State> m_state;
    };
}  // namespace Horo::Editor
