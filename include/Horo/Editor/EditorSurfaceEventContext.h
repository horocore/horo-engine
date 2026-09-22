#pragma once

/**
 * @file EditorSurfaceEventContext.h
 * @brief Restricted, provider-owned editor event subscriptions for extension surfaces.
 */

#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Foundation/EditorEventTypes.h"
#include "Horo/Foundation/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <utility>

namespace Horo::Editor {
    /** @brief Copied activation identity used to attribute a surface's subscriptions. */
    struct EditorSurfaceProviderOwnership final {
        std::string extensionId;
        std::string moduleId;
        std::uint64_t activationGeneration{};

        /** @return True when this identity can own a live editor surface context. */
        [[nodiscard]] bool IsValid() const noexcept {
            return !extensionId.empty() && !moduleId.empty() && activationGeneration != 0;
        }
    };

    /** @brief Host-owned bounds for one restricted editor surface event context. */
    struct EditorSurfaceEventContextLimits final {
        std::size_t maximumSubscriptions{16};
    };

    /** @brief Observable lifecycle counters for one provider-owned event context. */
    struct EditorSurfaceEventContextStats final {
        std::size_t activeSubscriptions{};
        std::size_t acceptedSubscriptions{};
        std::size_t rejectedSubscriptions{};
        std::uint64_t deliveredEvents{};
        bool closed{};
    };

    /** @brief Variant envelope used by the generic restricted event callback. */
    using EditorSurfaceEventPayload =
        std::variant<EditorProjectOpenedEvent, EditorProjectClosedEvent, EditorAssetImportedEvent, EditorAssetReloadedEvent,
                     EditorOperationStoreRevisionChangedEvent, EditorConsoleLogEvent, EditorMetricsChangedEvent,
                     EditorProfilerCaptureStateEvent, EditorMcpToolInvocationEvent>;

    /** @brief One sanitized editor event delivered to an admitted surface provider. */
    struct EditorSurfaceEvent final {
        EditorEventKind kind{EditorEventKind::ProjectOpened};
        EditorSurfaceEventPayload payload{EditorProjectOpenedEvent{}};
    };

    /** @brief Callback signature for generic restricted surface events. */
    using EditorSurfaceEventHandler = std::function<void(const EditorSurfaceEvent &)>;

    namespace detail {
        template <typename EventT> struct EditorSurfaceEventTraits;

        template <> struct EditorSurfaceEventTraits<EditorProjectOpenedEvent> {
            static constexpr EditorEventKind Kind = EditorEventKind::ProjectOpened;
        };
        template <> struct EditorSurfaceEventTraits<EditorProjectClosedEvent> {
            static constexpr EditorEventKind Kind = EditorEventKind::ProjectClosed;
        };
        template <> struct EditorSurfaceEventTraits<EditorAssetImportedEvent> {
            static constexpr EditorEventKind Kind = EditorEventKind::AssetImported;
        };
        template <> struct EditorSurfaceEventTraits<EditorAssetReloadedEvent> {
            static constexpr EditorEventKind Kind = EditorEventKind::AssetReloaded;
        };
        template <> struct EditorSurfaceEventTraits<EditorOperationStoreRevisionChangedEvent> {
            static constexpr EditorEventKind Kind = EditorEventKind::OperationStoreRevisionChanged;
        };
        template <> struct EditorSurfaceEventTraits<EditorConsoleLogEvent> {
            static constexpr EditorEventKind Kind = EditorEventKind::ConsoleLog;
        };
        template <> struct EditorSurfaceEventTraits<EditorMetricsChangedEvent> {
            static constexpr EditorEventKind Kind = EditorEventKind::MetricsChanged;
        };
        template <> struct EditorSurfaceEventTraits<EditorProfilerCaptureStateEvent> {
            static constexpr EditorEventKind Kind = EditorEventKind::ProfilerCaptureState;
        };
        template <> struct EditorSurfaceEventTraits<EditorMcpToolInvocationEvent> {
            static constexpr EditorEventKind Kind = EditorEventKind::McpToolInvocation;
        };

        template <typename EventT>
        concept EditorSurfaceEventType = requires {
            EditorSurfaceEventTraits<EventT>::Kind;
        };
    }  // namespace detail

    /**
     * @brief Provider-owned, bounded subscription surface for sanitized editor events.
     *
     * This context intentionally exposes no publish operation and no underlying bus. Closing
     * it revokes every subscription immediately; tokens retained by a provider become inert.
     */
    class EditorSurfaceEventContext final {
    public:
        /**
         * @brief Creates a context with an explicit event admission set.
         * @param editorEvents Session-owned editor bus.
         * @param provider Exact extension/module activation that owns the context.
         * @param allowedEvents Host-approved event kinds; the span is copied.
         * @param limits Bounded subscription policy.
         */
        EditorSurfaceEventContext(EditorDataBus &editorEvents, EditorSurfaceProviderOwnership provider,
                                  std::span<const EditorEventKind> allowedEvents,
                                  EditorSurfaceEventContextLimits limits = {});
        /** @brief Revokes all subscriptions before the provider context is released. */
        ~EditorSurfaceEventContext();
        EditorSurfaceEventContext(const EditorSurfaceEventContext &) = delete;
        EditorSurfaceEventContext &operator=(const EditorSurfaceEventContext &) = delete;
        /** @brief Transfers the provider context without duplicating its subscriptions. */
        EditorSurfaceEventContext(EditorSurfaceEventContext &&other) noexcept;
        /** @brief Closes the current context and transfers the provider context. */
        EditorSurfaceEventContext &operator=(EditorSurfaceEventContext &&other) noexcept;

        /**
         * @brief Subscribes to one admitted sanitized event identity.
         * @param kind Event identity granted by the host.
         * @param handler Generic callback receiving the safe event envelope.
         * @return A revocable token, or a typed rejection for closed, unallowed, or over-capacity requests.
         */
        [[nodiscard]] Result<Subscription> Subscribe(EditorEventKind kind, EditorSurfaceEventHandler handler);

        /**
         * @brief Subscribes to one admitted event with its concrete sanitized payload type.
         * @tparam EventT One of the editor payload types declared by this contract.
         * @tparam Handler Callable accepting `const EventT&`.
         * @param handler Typed callback.
         * @return A revocable token or a typed admission failure.
         */
        template <detail::EditorSurfaceEventType EventT, typename Handler>
        [[nodiscard]] Result<Subscription> Subscribe(Handler &&handler) {
            auto callback = std::forward<Handler>(handler);
            return Subscribe(detail::EditorSurfaceEventTraits<EventT>::Kind,
                             [callback = std::move(callback)](const EditorSurfaceEvent &event) mutable {
                                 if (const auto *payload = std::get_if<EventT>(&event.payload))
                                     callback(*payload);
                             });
        }

        /** @brief Revokes all provider subscriptions and makes this context permanently closed. */
        void Close() noexcept;
        /** @return True after explicit close, invalid ownership, or destruction has begun. */
        [[nodiscard]] bool IsClosed() const noexcept;
        /** @return A race-safe snapshot of provider-context admission and delivery counters. */
        [[nodiscard]] EditorSurfaceEventContextStats Stats() const noexcept;
        /** @return The copied provider activation identity associated with this context. */
        [[nodiscard]] EditorSurfaceProviderOwnership ProviderOwnership() const;

    private:
        struct Slot;
        struct State;
        std::shared_ptr<State> m_state;
    };

    /** @brief Compatibility name used by host documentation for a provider's event session. */
    using EditorSurfaceEventSession = EditorSurfaceEventContext;
}  // namespace Horo::Editor
