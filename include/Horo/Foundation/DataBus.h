#pragma once

/**
 * @file DataBus.h
 * @brief Bounded typed process notification bus and revocable subscriptions.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Horo {
    using EventTypeId = std::uint64_t;

    /** @brief Polymorphic transport base for a typed event payload owned by the bus operation. */
    struct EventPayload {
        virtual ~EventPayload() = default;
    };

    template <typename EventT> struct TypedEventPayload final : EventPayload {
        explicit TypedEventPayload(const EventT &event) : value(event) {}

        explicit TypedEventPayload(EventT &&event) : value(std::move(event)) {}

        EventT value;
    };

    /** @brief Hashes an explicit, stable event name with FNV-1a. */
    constexpr EventTypeId HashEventTypeName(const std::string_view name) noexcept {
        EventTypeId hash = 14695981039346656037ULL;
        for (const char character : name) {
            hash ^= static_cast<EventTypeId>(static_cast<unsigned char>(character));
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    template <typename EventT> [[nodiscard]] constexpr EventTypeId EventType() noexcept {
        return HashEventTypeName(EventT::HoroEventTypeName);
    }

    /** @brief Move-only RAII ownership of one data-bus handler. */
    class Subscription {
    public:
        Subscription() = default;
        ~Subscription();
        Subscription(const Subscription &) = delete;
        Subscription &operator=(const Subscription &) = delete;
        Subscription(Subscription &&other) noexcept;
        Subscription &operator=(Subscription &&other) noexcept;
        void Reset() noexcept;

        /**
         * @brief Adopts a revocation callback into a move-only subscription token.
         * @param release Callback that revokes the owned registration; it is invoked at most once.
         * @return A token that invokes `release` when reset or destroyed.
         * @note This is for host-owned adapters that compose subscriptions from another typed surface.
         */
        [[nodiscard]] static Subscription Adopt(std::function<void()> release);

        [[nodiscard]] explicit operator bool() const noexcept {
            return static_cast<bool>(m_release);
        }

    private:
        friend class EngineDataBus;

        explicit Subscription(std::function<void()> release) : m_release(std::move(release)) {}

        std::function<void()> m_release;
    };

    /** @brief Policy applied when an asynchronous event queue reaches its bound. */
    enum class BackpressurePolicy : std::uint8_t {
        DropNewest,
        DropOldest,
        Merge,
    };

    struct EngineDataBusConfig {
        std::size_t maxAsyncQueueSize = 1024;
        bool traceDispatch = true;
        const char *logCategory = "engine.data_bus";
        std::size_t maxSubscriptions = 1024;
        BackpressurePolicy defaultBackpressurePolicy = BackpressurePolicy::DropNewest;
        std::unordered_map<EventTypeId, BackpressurePolicy> eventBackpressurePolicies;
    };

    /** @brief Observable counters for the bounded asynchronous event queue. */
    struct EngineDataBusQueueStats final {
        std::size_t queued{};
        std::size_t activeSubscriptions{};
        std::uint64_t enqueued{};
        std::uint64_t dispatched{};
        std::uint64_t droppedNewest{};
        std::uint64_t droppedOldest{};
        std::uint64_t merged{};
    };

    /** @brief Process-scoped typed notification bus; commands and state ownership remain external. */
    class EngineDataBus {
    public:
        explicit EngineDataBus(EngineDataBusConfig config = {});
        ~EngineDataBus();
        EngineDataBus(const EngineDataBus &) = delete;
        EngineDataBus &operator=(const EngineDataBus &) = delete;
        EngineDataBus(EngineDataBus &&) noexcept;
        EngineDataBus &operator=(EngineDataBus &&) noexcept;

        template <typename EventT, typename Handler> Subscription Subscribe(Handler &&handler) {
            static_assert(requires { EventT::HoroEventTypeName; }, "Events require a stable HoroEventTypeName.");
            auto typed = [callback = std::forward<Handler>(handler)](const EventPayload *raw) {
                const auto *payload = dynamic_cast<const TypedEventPayload<EventT> *>(raw);
                if (payload != nullptr)
                    callback(payload->value);
            };
            return SubscribeErased(EventType<EventT>(), EventT::HoroEventTypeName, std::move(typed));
        }

        template <typename EventT> void Publish(const EventT &event) {
            static_assert(requires { EventT::HoroEventTypeName; }, "Events require a stable HoroEventTypeName.");
            auto replay = std::make_shared<TypedEventPayload<EventT>>(event);
            const auto *raw = replay.get();
            PublishErased(EventType<EventT>(), EventT::HoroEventTypeName, raw, [replay = std::move(replay)](EngineDataBus &bus) {
                bus.Publish(replay->value);
            });
        }

        template <typename EventT> void PublishAsync(EventT event) {
            static_assert(requires { EventT::HoroEventTypeName; }, "Events require a stable HoroEventTypeName.");
            auto payload = std::make_shared<TypedEventPayload<EventT>>(std::move(event));
            QueueErased(EventType<EventT>(), EventT::HoroEventTypeName, std::move(payload),
                        [](EngineDataBus &bus, const EventPayload *raw) {
                const auto *typed = dynamic_cast<const TypedEventPayload<EventT> *>(raw);
                if (typed != nullptr)
                    bus.Publish(typed->value);
            });
        }

        /** @brief Delivers worker-thread notifications at the owner synchronization point. */
        void DispatchQueued();
        /** @brief Removes all handlers and queued notifications. */
        void Clear();
        /** @brief Returns a race-safe snapshot of queue pressure and registration counts. */
        [[nodiscard]] EngineDataBusQueueStats QueueStats() const;

    private:
        using Handler = std::function<void(const EventPayload *)>;
        using DeferredPublisher = std::function<void(EngineDataBus &)>;
        using QueuedPublisher = std::function<void(EngineDataBus &, const EventPayload *)>;
        struct State;
        Subscription SubscribeErased(EventTypeId type, std::string_view name, Handler handler);
        void PublishErased(EventTypeId type, std::string_view name, const EventPayload *raw, DeferredPublisher retry);
        void QueueErased(EventTypeId type, std::string_view name, std::shared_ptr<const EventPayload> payload, QueuedPublisher publish);
        std::shared_ptr<State> m_state;
    };
}  // namespace Horo
