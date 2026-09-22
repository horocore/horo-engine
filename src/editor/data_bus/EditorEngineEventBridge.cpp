#include "Horo/Editor/EditorEngineEventBridge.h"

#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/ProcessEvents.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <string_view>
#include <thread>

namespace Horo::Editor {
    namespace {
        constexpr std::size_t kAllowlistedEventCount = 9;
        constexpr std::size_t kMaximumAssetIdentityBytes = 256;

        /** @brief Accepts only bounded path-free canonical asset identities. */
        [[nodiscard]] bool IsSafeAssetIdentity(const std::string_view value) noexcept {
            if (value.empty() || value.size() > kMaximumAssetIdentityBytes)
                return false;
            for (const unsigned char character : value) {
                const bool alphaNumeric = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
                if (!alphaNumeric && character != '.' && character != '_' && character != '-')
                    return false;
            }
            return true;
        }

        /** @brief Rejects invalid enum representations before publishing a safe operation hint. */
        [[nodiscard]] bool IsKnownOperationState(const OperationState state) noexcept {
            return static_cast<std::uint8_t>(state) <= static_cast<std::uint8_t>(OperationState::Cancelled);
        }

        /** @brief Rejects invalid enum representations before publishing a safe profiler hint. */
        [[nodiscard]] bool IsKnownProfilerState(const ProfilerCaptureState state) noexcept {
            return static_cast<std::uint8_t>(state) <= static_cast<std::uint8_t>(ProfilerCaptureState::Failed);
        }
    }  // namespace

    struct EditorEngineEventBridge::State final {
        EngineDataBus &engineEvents;
        EditorDataBus &editorEvents;
        const std::thread::id ownerThread{std::this_thread::get_id()};
        std::array<Subscription, kAllowlistedEventCount> subscriptions;
        std::atomic<bool> attached{false};
        std::atomic<std::uint64_t> forwarded{};
        std::atomic<std::uint64_t> filtered{};
        std::atomic<std::uint64_t> threadDrops{};

        State(EngineDataBus &engine, EditorDataBus &editor) : engineEvents(engine), editorEvents(editor) {}

        [[nodiscard]] bool CanForward() noexcept {
            if (std::this_thread::get_id() != ownerThread) {
                threadDrops.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            return attached.load(std::memory_order_acquire);
        }

        template <typename EventT> void SubscribeOne(const std::weak_ptr<State> &weakState, Subscription &target) {
            target = engineEvents.Subscribe<EventT>([weakState](const auto &event) {
                if (const auto state = weakState.lock())
                    state->Forward(event);
            });
        }

        [[nodiscard]] bool SubscribeAll(const std::weak_ptr<State> &weakState) {
            SubscribeOne<Horo::ProjectOpenedEvent>(weakState, subscriptions[0]);
            SubscribeOne<Horo::ProjectClosedEvent>(weakState, subscriptions[1]);
            SubscribeOne<Horo::AssetImportedEvent>(weakState, subscriptions[2]);
            SubscribeOne<Horo::AssetReloadedEvent>(weakState, subscriptions[3]);
            SubscribeOne<Horo::OperationStoreRevisionChangedEvent>(weakState, subscriptions[4]);
            SubscribeOne<Horo::ConsoleLogEvent>(weakState, subscriptions[5]);
            SubscribeOne<Horo::MetricsChangedEvent>(weakState, subscriptions[6]);
            SubscribeOne<Horo::ProfilerCaptureStateEvent>(weakState, subscriptions[7]);
            SubscribeOne<Horo::McpToolInvocationEvent>(weakState, subscriptions[8]);
            return std::ranges::all_of(subscriptions, [](const Subscription &subscription) {
                return static_cast<bool>(subscription);
            });
        }

        void ResetSubscriptions() noexcept {
            for (Subscription &subscription : subscriptions)
                subscription.Reset();
        }

        void Forward(const Horo::ProjectOpenedEvent &event) {
            if (!CanForward())
                return;
            editorEvents.Publish(EditorProjectOpenedEvent{.revision = event.revision});
            forwarded.fetch_add(1, std::memory_order_relaxed);
        }

        void Forward(const Horo::ProjectClosedEvent &event) {
            if (!CanForward())
                return;
            editorEvents.Publish(EditorProjectClosedEvent{.revision = event.revision});
            forwarded.fetch_add(1, std::memory_order_relaxed);
        }

        void Forward(const Horo::AssetImportedEvent &event) {
            if (!CanForward())
                return;
            if (!IsSafeAssetIdentity(event.assetId)) {
                filtered.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            editorEvents.Publish(EditorAssetImportedEvent{.revision = event.revision, .assetId = event.assetId});
            forwarded.fetch_add(1, std::memory_order_relaxed);
        }

        void Forward(const Horo::AssetReloadedEvent &event) {
            if (!CanForward())
                return;
            if (!IsSafeAssetIdentity(event.assetId)) {
                filtered.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            editorEvents.Publish(EditorAssetReloadedEvent{.revision = event.revision, .assetId = event.assetId});
            forwarded.fetch_add(1, std::memory_order_relaxed);
        }

        void Forward(const Horo::OperationStoreRevisionChangedEvent &event) {
            if (!CanForward())
                return;
            if (!IsKnownOperationState(event.state)) {
                filtered.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            editorEvents.Publish(EditorOperationStoreRevisionChangedEvent{
                .revision = event.revision,
                .changedOperation = event.changedOperation,
                .state = event.state,
            });
            forwarded.fetch_add(1, std::memory_order_relaxed);
        }

        void Forward(const Horo::ConsoleLogEvent &event) {
            if (!CanForward())
                return;
            editorEvents.Publish(EditorConsoleLogEvent{.revision = event.revision, .appendedCount = event.appendedCount});
            forwarded.fetch_add(1, std::memory_order_relaxed);
        }

        void Forward(const Horo::MetricsChangedEvent &event) {
            if (!CanForward())
                return;
            editorEvents.Publish(EditorMetricsChangedEvent{.revision = event.revision, .changedGroups = event.changedGroups});
            forwarded.fetch_add(1, std::memory_order_relaxed);
        }

        void Forward(const Horo::ProfilerCaptureStateEvent &event) {
            if (!CanForward())
                return;
            if (!IsKnownProfilerState(event.state)) {
                filtered.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            editorEvents.Publish(EditorProfilerCaptureStateEvent{.captureId = event.captureId, .state = event.state});
            forwarded.fetch_add(1, std::memory_order_relaxed);
        }

        void Forward(const Horo::McpToolInvocationEvent &event) {
            if (!CanForward())
                return;
            if (!IsKnownOperationState(event.state)) {
                filtered.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            editorEvents.Publish(EditorMcpToolInvocationEvent{.state = event.state, .historyRevision = event.historyRevision});
            forwarded.fetch_add(1, std::memory_order_relaxed);
        }
    };

    /** @copydoc EditorEngineEventBridge::EditorEngineEventBridge */
    EditorEngineEventBridge::EditorEngineEventBridge(EngineDataBus &engineEvents, EditorDataBus &editorEvents)
        : m_state(std::make_shared<State>(engineEvents, editorEvents)) {}

    /** @copydoc EditorEngineEventBridge::~EditorEngineEventBridge */
    EditorEngineEventBridge::~EditorEngineEventBridge() {
        Detach();
    }

    /** @copydoc EditorEngineEventBridge::Attach */
    void EditorEngineEventBridge::Attach() {
        assert(m_state != nullptr);
        if (m_state == nullptr || std::this_thread::get_id() != m_state->ownerThread)
            return;
        if (m_state->attached.load(std::memory_order_acquire))
            return;

        const std::weak_ptr<State> weakState = m_state;
        if (!m_state->SubscribeAll(weakState)) {
            m_state->ResetSubscriptions();
            LOG_WARN("editor.data_bus", "process bridge attach rejected reason=subscription_limit");
            return;
        }
        m_state->attached.store(true, std::memory_order_release);
    }

    /** @copydoc EditorEngineEventBridge::Detach */
    void EditorEngineEventBridge::Detach() noexcept {
        if (m_state == nullptr)
            return;
        m_state->attached.store(false, std::memory_order_release);
        m_state->ResetSubscriptions();
    }

    /** @copydoc EditorEngineEventBridge::IsAttached */
    bool EditorEngineEventBridge::IsAttached() const noexcept {
        return m_state != nullptr && m_state->attached.load(std::memory_order_acquire);
    }

    /** @copydoc EditorEngineEventBridge::Stats */
    EditorEngineEventBridgeStats EditorEngineEventBridge::Stats() const noexcept {
        if (m_state == nullptr)
            return {};
        return EditorEngineEventBridgeStats{
            .forwarded = m_state->forwarded.load(std::memory_order_relaxed),
            .filtered = m_state->filtered.load(std::memory_order_relaxed),
            .threadDrops = m_state->threadDrops.load(std::memory_order_relaxed),
        };
    }
}  // namespace Horo::Editor
