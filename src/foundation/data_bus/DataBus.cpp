#include "Horo/Foundation/DataBus.h"

#include "Horo/Foundation/Logging/Logger.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <mutex>
#include <ranges>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Horo {
    struct EngineDataBus::State {
        struct Record {
            std::uint64_t id;
            Handler handler;
        };

        struct Queued {
            EventTypeId type;
            std::string name;
            std::shared_ptr<const EventPayload> payload;
            QueuedPublisher publish;
        };

        EngineDataBusConfig config;
        std::mutex mutex;
        std::unordered_map<EventTypeId, std::vector<Record>> handlers;
        std::deque<Queued> queued;
        std::vector<EventTypeId> activeTypes;
        std::vector<DeferredPublisher> deferred;
        std::uint64_t nextId = 1;
        std::size_t activeSubscriptionCount{};
        std::uint64_t enqueued{};
        std::uint64_t dispatched{};
        std::uint64_t droppedNewest{};
        std::uint64_t droppedOldest{};
        std::uint64_t merged{};

        explicit State(EngineDataBusConfig value) : config(value) {}
    };

    Subscription::~Subscription() {
        Reset();
    }

    Subscription::Subscription(Subscription &&other) noexcept : m_release(std::move(other.m_release)) {
        other.m_release = nullptr;
    }

    Subscription &Subscription::operator=(Subscription &&other) noexcept {
        if (this != &other) {
            Reset();
            m_release = std::move(other.m_release);
            other.m_release = nullptr;
        }
        return *this;
    }

    void Subscription::Reset() noexcept {
        if (m_release) {
            m_release();
            m_release = {};
        }
    }

    /** @copydoc Subscription::Adopt */
    Subscription Subscription::Adopt(std::function<void()> release) {
        return Subscription(std::move(release));
    }

    EngineDataBus::EngineDataBus(EngineDataBusConfig config) : m_state(std::make_shared<State>(config)) {}

    EngineDataBus::~EngineDataBus() {
        Clear();
    }

    EngineDataBus::EngineDataBus(EngineDataBus &&) noexcept = default;
    EngineDataBus &EngineDataBus::operator=(EngineDataBus &&) noexcept = default;

    Subscription EngineDataBus::SubscribeErased(const EventTypeId type, const std::string_view name,  // NOSONAR(cpp:S5817)
                                                Handler handler) {
        const auto state = m_state;
        std::uint64_t id = 0;
        {
            std::lock_guard lock(state->mutex);
            if (state->activeSubscriptionCount >= state->config.maxSubscriptions) {
                LOG_WARN(state->config.logCategory, "subscribe rejected event=%s reason=subscription_limit", name.data());
                return {};
            }
            id = state->nextId++;
            state->handlers[type].emplace_back(id, std::move(handler));
            ++state->activeSubscriptionCount;
        }
        LOG_TRACE(state->config.logCategory, "subscribe event=%s handler=%llu", name.data(), static_cast<unsigned long long>(id));
        return Subscription(
            [weak = std::weak_ptr<State>(state), type, id, category = state->config.logCategory, eventName = std::string(name)] {
            if (const auto locked = weak.lock()) {
                std::lock_guard lock(locked->mutex);
                if (const auto it = locked->handlers.find(type); it != locked->handlers.end()) {
                    const std::size_t before = it->second.size();
                    std::erase_if(it->second, [id](const State::Record &record) {
                        return record.id == id;
                    });
                    locked->activeSubscriptionCount -= before - it->second.size();
                    if (it->second.empty())
                        locked->handlers.erase(it);
                }
                LOG_TRACE(category, "unsubscribe event=%s handler=%llu", eventName.c_str(), static_cast<unsigned long long>(id));
            }
        });
    }

    void EngineDataBus::PublishErased(const EventTypeId type, const std::string_view name, const EventPayload *raw,
                                      DeferredPublisher retry) {
        const auto state = m_state;
        if (std::ranges::find(state->activeTypes, type) != state->activeTypes.end()) {
            state->deferred.push_back(std::move(retry));
            return;
        }
        std::vector<Handler> snapshot;
        {
            std::lock_guard lock(state->mutex);
            if (const auto it = state->handlers.find(type); it != state->handlers.end())
                for (const auto &record : it->second)
                    snapshot.push_back(record.handler);
        }
        const auto start = std::chrono::steady_clock::now();
        LOG_TRACE(state->config.logCategory, "publish event=%s handlers=%zu", name.data(), snapshot.size());
        state->activeTypes.push_back(type);
        for (const auto &handler : snapshot) {
            try {
                handler(raw);
            } catch (const std::runtime_error &exception) {  // NOSONAR(cpp:S1181)
                LOG_ERROR(state->config.logCategory, "handler failed event=%s error=%s", name.data(), exception.what());
            } catch (const std::logic_error &exception) {  // NOSONAR(cpp:S1181)
                LOG_ERROR(state->config.logCategory, "handler failed event=%s error=%s", name.data(), exception.what());
            } catch (const std::bad_alloc &exception) {  // NOSONAR(cpp:S1181)
                LOG_ERROR(state->config.logCategory, "handler failed event=%s error=%s", name.data(), exception.what());
            } catch (const std::exception &exception) {  // NOSONAR(cpp:S1181)
                LOG_ERROR(state->config.logCategory, "handler failed event=%s error=%s", name.data(), exception.what());
            } catch (...) {  // NOSONAR(cpp:S1181)
                LOG_ERROR(state->config.logCategory, "handler failed event=%s error=unknown", name.data());
            }
        }
        state->activeTypes.pop_back();
        LOG_TRACE(state->config.logCategory, "dispatch complete event=%s elapsed_us=%lld", name.data(),
                  std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        if (state->activeTypes.empty()) {
            auto deferred = std::move(state->deferred);
            state->deferred.clear();
            for (const auto &publish : deferred)
                publish(*this);
        }
    }

    void EngineDataBus::QueueErased(const EventTypeId type, const std::string_view name,  // NOSONAR(cpp:S5817)
                                    std::shared_ptr<const EventPayload> payload, QueuedPublisher publish) {
        std::lock_guard lock(m_state->mutex);
        const auto dropNewest = [&] {
            ++m_state->droppedNewest;
            LOG_TRACE(m_state->config.logCategory, "async drop event=%s reason=queue_full policy=drop_newest", name.data());
        };
        if (m_state->config.maxAsyncQueueSize == 0) {
            dropNewest();
            return;
        }

        const auto policy = [&] {
            if (const auto it = m_state->config.eventBackpressurePolicies.find(type);
                it != m_state->config.eventBackpressurePolicies.end())
                return it->second;
            return m_state->config.defaultBackpressurePolicy;
        }();
        if (m_state->queued.size() >= m_state->config.maxAsyncQueueSize) {
            switch (policy) {
                case BackpressurePolicy::DropNewest:
                    dropNewest();
                    return;
                case BackpressurePolicy::DropOldest:
                    m_state->queued.pop_front();
                    ++m_state->droppedOldest;
                    LOG_TRACE(m_state->config.logCategory, "async drop event=%s reason=queue_full policy=drop_oldest", name.data());
                    break;
                case BackpressurePolicy::Merge: {
                    const auto existing = std::ranges::find_if(m_state->queued.rbegin(), m_state->queued.rend(), [type](const State::Queued &queued) {
                        return queued.type == type;
                    });
                    if (existing == m_state->queued.rend()) {
                        dropNewest();
                        return;
                    }
                    existing->payload = std::move(payload);
                    existing->publish = std::move(publish);
                    ++m_state->merged;
                    LOG_TRACE(m_state->config.logCategory, "async merge event=%s reason=queue_full", name.data());
                    return;
                }
            }
        }
        m_state->queued.emplace_back(type, std::string(name), std::move(payload), std::move(publish));
        ++m_state->enqueued;
    }

    void EngineDataBus::DispatchQueued() {
        std::deque<State::Queued> queued;
        {
            std::lock_guard lock(m_state->mutex);
            queued.swap(m_state->queued);
            m_state->dispatched += queued.size();
        }
        for (const auto &event : queued)
            event.publish(*this, event.payload.get());
    }

    void EngineDataBus::Clear() {  // NOSONAR(cpp:S5817)
        if (!m_state)
            return;
        std::lock_guard lock(m_state->mutex);
        m_state->handlers.clear();
        m_state->queued.clear();
        m_state->activeSubscriptionCount = 0;
    }

    /** @copydoc EngineDataBus::QueueStats */
    EngineDataBusQueueStats EngineDataBus::QueueStats() const {
        if (!m_state)
            return {};
        std::lock_guard lock(m_state->mutex);
        return EngineDataBusQueueStats{
            .queued = m_state->queued.size(),
            .activeSubscriptions = m_state->activeSubscriptionCount,
            .enqueued = m_state->enqueued,
            .dispatched = m_state->dispatched,
            .droppedNewest = m_state->droppedNewest,
            .droppedOldest = m_state->droppedOldest,
            .merged = m_state->merged,
        };
    }

}  // namespace Horo
